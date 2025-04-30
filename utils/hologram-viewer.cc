// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Copyright (C) 2015 Henner Zeller <h.zeller@acm.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

// To use this image viewer, first get image-magick development files
// $ sudo apt-get install libgraphicsmagick++-dev libwebp-dev
//
// Then compile with
// $ make hologram-viewer

#include "led-matrix.h"
#include "pixel-mapper.h"
#include "content-streamer.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <pthread.h>

#include <Magick++.h>
#include <magick/image.h>

// https://blog.mbedded.ninja/programming/operating-systems/linux/linux-serial-ports-using-c-cpp/
#include <sys/ioctl.h>
#include <errno.h>
#include <termios.h>

using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::StreamReader;

#define SPIN_SYNC 3 // gpio
#define UART_BUF_SIZE 8192 // 1024 should be enough but not by much

typedef int64_t tmillis_t;
static const tmillis_t distant_future = (1LL<<40); // that is a while.

static volatile char* anim_state = NULL;
static int serial_fd;
static uint8_t read_buf[UART_BUF_SIZE]; // rolling UART buffer
static volatile size_t uart_buf_head = 0; // length of stored buffer
static volatile bool read_busy = 0; // if process_uart is running

static pthread_t uart_thread;

static RGBMatrix *matrix;
static FrameCanvas *offscreen_canvas;

namespace fs = std::filesystem;

#define IMAGE_DIR "/hologram/utils/images/"

struct ImageParams {
  ImageParams() : anim_duration_ms(distant_future), wait_ms(1500),
                  anim_delay_ms(-1), loops(-1), vsync_multiple(1) {}
  tmillis_t anim_duration_ms;  // If this is an animation, duration to show.
  tmillis_t wait_ms;           // Regular image: duration to show.
  tmillis_t anim_delay_ms;     // Animation delay override.
  int loops;
  int vsync_multiple;
};

struct FileInfo {
  ImageParams params;      // Each file might have specific timing settings
  bool is_multi_frame = false;
  rgb_matrix::StreamIO *content_stream = nullptr;
};

struct AnimState {
  char *name;
  std::vector< rgb_matrix::StreamIO* > play_streams;
  std::vector< rgb_matrix::StreamIO* > idle_streams;
  int loop_point = 0;
};

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
  if(uart_thread > 0)
  {
    pthread_cancel(uart_thread);
  }
}

volatile bool do_reset = false;
static void ResetHandler(int signo) {
  do_reset = true;
}

static tmillis_t GetTimeInMillis() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static void SleepMillis(tmillis_t milli_seconds) {
  if (milli_seconds <= 0) return;
  struct timespec ts;
  ts.tv_sec = milli_seconds / 1000;
  ts.tv_nsec = (milli_seconds % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

static void StoreInStream(const Magick::Image &img, int delay_time_us,
                          bool do_center,
                          rgb_matrix::FrameCanvas *scratch,
                          rgb_matrix::StreamWriter *output) {
  scratch->Clear();
  const int x_offset = do_center ? (scratch->width() - img.columns()) / 2 : 0;
  const int y_offset = do_center ? (scratch->height() - img.rows()) / 2 : 0;
  for (size_t y = 0; y < img.rows(); ++y) {
    for (size_t x = 0; x < img.columns(); ++x) {
      const Magick::Color &c = img.pixelColor(x, y);
      if (c.alphaQuantum() < 255) {
        scratch->SetPixel(x + x_offset, y + y_offset,
                          ScaleQuantumToChar(c.redQuantum()),
                          ScaleQuantumToChar(c.greenQuantum()),
                          ScaleQuantumToChar(c.blueQuantum()));
      }
    }
  }
  output->Stream(*scratch, delay_time_us);
}

static void CopyStream(rgb_matrix::StreamReader *r,
                       rgb_matrix::StreamWriter *w,
                       rgb_matrix::FrameCanvas *scratch) {
  uint32_t delay_us;
  while (r->GetNext(scratch, &delay_us)) {
    w->Stream(*scratch, delay_us);
  }
}

// Load still image or animation.
// Scale, so that it fits in "width" and "height" and store in "result".
static bool LoadImageAndScale(const char *filename,
                              int target_width, int target_height,
                              bool fill_width, bool fill_height,
                              std::vector<Magick::Image> *result,
                              std::string *err_msg) {
  std::vector<Magick::Image> frames;
  try {
    readImages(&frames, filename);
  } catch (std::exception& e) {
    if (e.what()) *err_msg = e.what();
    return false;
  }
  if (frames.size() == 0) {
    fprintf(stderr, "No image found.");
    return false;
  }

  // Put together the animation from single frames. GIFs can have nasty
  // disposal modes, but they are handled nicely by coalesceImages()
  if (frames.size() > 1) {
    Magick::coalesceImages(result, frames.begin(), frames.end());
  } else {
    result->push_back(frames[0]);   // just a single still image.
  }

  const int img_width = (*result)[0].columns();
  const int img_height = (*result)[0].rows();
  const float width_fraction = (float)target_width / img_width;
  const float height_fraction = (float)target_height / img_height;
  if (fill_width && fill_height) {
    // Scrolling diagonally. Fill as much as we can get in available space.
    // Largest scale fraction determines that.
    const float larger_fraction = (width_fraction > height_fraction)
      ? width_fraction
      : height_fraction;
    target_width = (int) roundf(larger_fraction * img_width);
    target_height = (int) roundf(larger_fraction * img_height);
  }
  else if (fill_height) {
    // Horizontal scrolling: Make things fit in vertical space.
    // While the height constraint stays the same, we can expand to full
    // width as we scroll along that axis.
    target_width = (int) roundf(height_fraction * img_width);
  }
  else if (fill_width) {
    // dito, vertical. Make things fit in horizontal space.
    target_height = (int) roundf(width_fraction * img_height);
  }

  for (size_t i = 0; i < result->size(); ++i) {
    (*result)[i].scale(Magick::Geometry(target_width, target_height));
  }

  return true;
}

void DisplayAnimation(const FileInfo *file,
                      RGBMatrix *m, FrameCanvas *o_c) {
  const tmillis_t duration_ms = (file->is_multi_frame
                                 ? file->params.anim_duration_ms
                                 : file->params.wait_ms);
  rgb_matrix::StreamReader reader(file->content_stream); // get the image stream
  int loops = file->params.loops;
  const tmillis_t end_time_ms = GetTimeInMillis() + duration_ms;
  const tmillis_t override_anim_delay = file->params.anim_delay_ms;
  for (int k = 0;
       (loops < 0 || k < loops)
         && !interrupt_received
         && GetTimeInMillis() < end_time_ms;
       ++k) {
    uint32_t delay_us = 0;
    while (!interrupt_received && GetTimeInMillis() <= end_time_ms
           && reader.GetNext(o_c, &delay_us)) {
      const tmillis_t anim_delay_ms =
        override_anim_delay >= 0 ? override_anim_delay : delay_us / 1000;
      const tmillis_t start_wait_ms = GetTimeInMillis();
      o_c = m->SwapOnVSync(o_c,
                                             file->params.vsync_multiple);
      const tmillis_t time_already_spent = GetTimeInMillis() - start_wait_ms;
      SleepMillis(anim_delay_ms - time_already_spent);
    }
    reader.Rewind();
  }
}

// store file in canvas stream
void do_magick(char* filename, char* state, bool* idle) //, int slice)
{
  ImageParams img_param;
  FileInfo *file_info = NULL;

  std::string err_msg;
  std::vector<Magick::Image> image_sequence;
  if (LoadImageAndScale(filename, matrix->width(), matrix->height(),
                        false, false, &image_sequence, &err_msg)) {
    file_info = new FileInfo();
    file_info->params = img_param;
    file_info->content_stream = new rgb_matrix::MemStreamIO();
    file_info->is_multi_frame = image_sequence.size() > 1;
    rgb_matrix::StreamWriter out(file_info->content_stream);
    for (size_t i = 0; i < image_sequence.size(); ++i) {
      const Magick::Image &img = image_sequence[i];
      StoreInStream(img, 0, false, offscreen_canvas, &out);
    }
  }
  if(file_info)
  {

  }
}

static int beginUART() {
  serial_fd = open("/dev/ttyS0", O_RDWR);
  struct termios tty;
  if(tcgetattr(serial_fd, &tty) != 0) {
      printf("Error %i from tcgetattr: %s\n", errno, strerror(errno));
      return -1;
  }
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= CREAD | CLOCAL;
  tty.c_lflag &= ~ICANON;
  tty.c_lflag &= ~ECHO;
  tty.c_lflag &= ~ECHOE;
  tty.c_lflag &= ~ECHONL;
  tty.c_lflag &= ~ISIG;
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);
  tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL);
  tty.c_oflag &= ~OPOST;
  tty.c_oflag &= ~ONLCR;
  tty.c_cc[VTIME] = 0;
  tty.c_cc[VMIN] = 0;
  cfsetispeed(&tty, B2000000);
  cfsetospeed(&tty, B2000000);
  if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
      printf("Error %i from tcsetattr: %s\n", errno, strerror(errno));
      return -1;
  }
  return serial_fd;
}

const char IDLE_SUFFIX[] = "_Idle";

static void process_uart_command(uint8_t cmd, uint8_t* payload, size_t len) {
  printf("PROCESS_UART_COMMAND : %d / %lu\n",cmd,len);
  if( cmd == 0x01 )
  {
    // receive an image
    printf("Received image with length %zu\n", len);

    // payload[0],[1] = payload size
    // Process the image data in payload
    uint16_t name_size = payload[2]; // big endian
    char filename[256]; // max POSIX filename length
    if (name_size >= sizeof(filename)) {
      // Filename too big for buffer
      return;
    }
    // Extract filename
    memcpy(filename, payload + 3, name_size);
    filename[name_size] = '\0'; // terminate the filename with a null character

    printf("%s\n",filename);

    // return;

    //////////////// ALL THIS JUST TO NAME THE DAMN FILE ////////////////
    // Find first numeric character in filename
    size_t base_len = 0;
    while (base_len < name_size && !(filename[base_len] >= '0' && filename[base_len] <= '9')) {
      base_len++;
    }
    if (base_len == 0) {
      // original filename is invalid as it contains numbers at the beginning
      // it actually isn't allowed to contain numbers at all EXCEPT the index at the end
      return;
    }
    char folder[512];
    char stateName[512];
    bool isIdle;
    snprintf(folder, sizeof(folder), "%s%.*s", IMAGE_DIR, (int)base_len, filename);

    // get state name & check if _Idle
    if( base_len > 6 && strcmp(folder + base_len - 5, IDLE_SUFFIX) == 0 )
    {
      memcpy(stateName, folder, base_len - 5);
      // do something
      isIdle = true;
    }
    else
    {
      memcpy(stateName, folder, base_len);
      isIdle = false;
    }

    // Make directory if it doesn't exist
    if (mkdir(folder, 0777) && errno != EEXIST) {
      printf("Failed to create folder: %s\nError: %s\n", folder, strerror(errno));
      return;
    }

    // Create full path to save file
    char fullpath[512];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", folder, filename);

    // Extract file contents
    size_t file_size = len - name_size - 3;
    uint8_t* file_contents = payload + 3 + name_size;

    // if ( !fs::exists(fullpath) )
    // {
      FILE* f = fopen(fullpath, "wb");
      if (f) {
        fwrite(file_contents, 1, file_size, f);
        fclose(f);
        printf("Saved file: %s (%zu bytes)\n", fullpath, file_size);
        do_magick(fullpath, stateName, isIdle); // store in canvas
      } else {
        printf("Failed to open file for writing: %s\n", fullpath);
      }
    // }
  }
  else if (cmd == 0x02)
  {
    // change animation state
    // 0x02 -> length [0] -> string -> \0

    uint8_t payload_len = payload[0];
    char nextState[payload_len+1];
    memcpy(nextState, payload+1, payload_len);

    nextState[payload_len] = '\0';
    printf("anim_state : %s\n",nextState);
  }
  else if (cmd == 0x03)
  {
    // receive a swipe input
    uint8_t payload_len = payload[0];
    char dir[payload_len+1]; // max length of direction string
    if (payload_len >= sizeof(dir)) {
      // Filename too big for buffer (should be impossible)
      return;
    }

    memcpy(dir, payload + 1, payload_len);
    dir[payload_len] = '\0'; // Terminate the string with a null character
    printf("Receiving swipe: %s\n", dir);

    for (uint8_t i = 0; i < payload_len; ++i) {
      char letter = dir[i];
      // @Johnathan back to you, i don't really know what you want to do with the swipe inputs.
      printf("Letter %d: %c\n", i, letter);
    }
  }
  fflush(stdout);
}

// poll UART and process if RX
static void *process_uart(void *arg)
{
  int bytes;
  while(!interrupt_received)
  {
    // todo: if time > timeout, reset uart_buf_head
    ioctl(serial_fd, FIONREAD, &bytes);
    if(bytes <= 0 )
    {
      continue;
    }

    printf("BYTES RECEIVED : %d\n",bytes);
    
    int r = read(serial_fd, read_buf + uart_buf_head, UART_BUF_SIZE - uart_buf_head); // read up to end of available buffer
    uart_buf_head += r;

    std::vector<uint8_t> full_read; // full command read only

    // get full command
    if( uart_buf_head > 0 )
    {
      full_read.push_back(read_buf[0]);

      uint16_t payload_len;
      int payhead = 0; // 

      if( full_read[0] == 0x01 ) // has 2-byte length
      {
        // read up until command + payload length
        while( uart_buf_head < 3 )
        {
          r = read(serial_fd, read_buf + uart_buf_head, UART_BUF_SIZE - uart_buf_head);
          uart_buf_head += r;
        }
        full_read.push_back(read_buf[1]);full_read.push_back(read_buf[2]);
        payload_len = (full_read[1] << 8) | full_read[2]; // big endian
        payhead = 2;
        printf("PAYLOAD : %u\n", payload_len);
      }
      else // 1-byte length
      {
        while( uart_buf_head < 2 )
        {
          r = read(serial_fd, read_buf + uart_buf_head, UART_BUF_SIZE - uart_buf_head);
          uart_buf_head += r;
        }
        full_read.push_back(read_buf[1]);
        payload_len = full_read[1];
        payhead = 1;
        printf("PAYLOAD : %u\n", payload_len);
      }

      int len = 0;
      // clear buffer head
      while( uart_buf_head > len + payhead + 1 && len < payload_len )
      {
        // printf("run %c %d %lu\n",read_buf[len+payhead+1], len, uart_buf_head);
        full_read.push_back(read_buf[len+payhead+1]);
        len++;
      }
      // move read buffer back if excess from last read
      if( len == payload_len )
      {
        uart_buf_head = uart_buf_head - (payload_len+payhead+1);
        memmove( read_buf, read_buf + (payload_len+payhead+1) , uart_buf_head );
      }
      while(len < payload_len)
      {
        r = read(serial_fd, read_buf, UART_BUF_SIZE);
        uart_buf_head += r;
        int c = 0;
        while(r > c && len < payload_len)
        {
          // printf("cont %c %lu\n",read_buf[c], uart_buf_head);
          full_read.push_back(read_buf[c++]);
          len++;
        }
        if( len == payload_len && r > c)
        {
          // printf("excess %d ",c);
          // for( int k = 0; k < r-c ; k++)
          // {
          //   printf("%c", read_buf[c+k]);
          // }
          uart_buf_head = r-c; // excess from last read
          memmove( read_buf, read_buf + c , uart_buf_head );
        }
        else
        {
          uart_buf_head = 0;
        }
      }
      fflush(stdout);


      process_uart_command(full_read[0], full_read.data() + 1, payhead+payload_len);
    }
  }
  pthread_exit(NULL);
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options] <image> [option] [<image> ...]\n",
          progname);

  fprintf(stderr, "Options:\n"
          "\t-O<streamfile>            : Output to stream-file instead of matrix (Don't need to be root).\n"
          "\t-C                        : Center images.\n"
          "\t-m                        : if this is a stream, mmap() it. This can work around IO latencies in SD-card and refilling kernel buffers. This will use physical memory so only use if you have enough to map file size\n"

          "\nThese options affect images FOLLOWING them on the command line,\n"
          "so it is possible to have different options for each image\n"
          "\t-w<seconds>               : Regular image: "
          "Wait time in seconds before next image is shown (default: 1.5).\n"
          "\t-t<seconds>               : "
          "For animations: stop after this time.\n"
          "\t-l<loop-count>            : "
          "For animations: number of loops through a full cycle.\n"
          "\t-D<animation-delay-ms>    : "
          "For animations: override the delay between frames given in the\n"
          "\t                            gif/stream animation with this value. Use -1 to use default value.\n"
          "\t-V<vsync-multiple>        : For animation (expert): Only do frame vsync-swaps on multiples of refresh (default: 1)\n"
          "\t                            (Tip: use --led-limit-refresh for stable rate)\n"

          "\nOptions affecting display of multiple images:\n"
          "\t-f                        : "
          "Forever cycle through the list of files on the command line.\n"
          "\t-s                        : If multiple images are given: shuffle.\n"
          );

  fprintf(stderr, "\nGeneral LED matrix options:\n");
  rgb_matrix::PrintMatrixFlags(stderr);

  fprintf(stderr,
          "\nSwitch time between files: "
          "-w for static images; -t/-l for animations\n"
          "Animated gifs: If both -l and -t are given, "
          "whatever finishes first determines duration.\n");

  fprintf(stderr, "\nThe -w, -t and -l options apply to the following images "
          "until a new instance of one of these options is seen.\n"
          "So you can choose different durations for different images.\n");

  return 1;
}

int main(int argc, char *argv[]) {
  Magick::InitializeMagick(*argv);

  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;

  // while( (serial_fd = open("/dev/ttyS0", O_RDONLY) ) < 0 )
  while( beginUART() < 0 )
  {
    fprintf( stderr, "UNABLE TO OPEN SERIAL");
    sleep(3);
  }

  // if( wiringPiSetup() == -1 )
  // {
  //   fprintf( stderr, "UNABLE TO START WIRINGPI");
  //   return 1;
  // }

  int idle_start = 0;
  // If started with 'sudo': make sure to drop privileges to same user
  // we started with, which is the most expected (and allows us to read
  // files as that user).
  runtime_opt.drop_priv_user = getenv("SUDO_UID");
  runtime_opt.drop_priv_group = getenv("SUDO_GID");
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  bool do_mmap = false;
  bool do_forever = false;
  bool do_center = false;
  bool do_shuffle = false;

  // We remember ImageParams for each image, which will change whenever
  // there is a flag modifying them. This map keeps track of filenames
  // and their image params (also for unrelated elements of argv[], but doesn't
  // matter).
  // We map the pointer instad of the string of the argv parameter so that
  // we can have two times the same image on the commandline list with different
  // parameters.
  std::map<const void *, struct ImageParams> filename_params;

  // Set defaults.
  ImageParams img_param;
  
  for (int i = 0; i < argc; ++i) {
    filename_params[argv[i]] = img_param;
  }

  const char *stream_output = NULL;

  int opt;
  while ((opt = getopt(argc, argv, "w:t:l:fr:c:P:LhCR:sO:V:D:m")) != -1) {
    switch (opt) {
    case 'w':
      img_param.wait_ms = roundf(atof(optarg) * 1000.0f);
      break;
    case 't':
      img_param.anim_duration_ms = roundf(atof(optarg) * 1000.0f);
      break;
    case 'l':
      img_param.loops = atoi(optarg);
      break;
    case 'D':
      img_param.anim_delay_ms = atoi(optarg);
      break;
    case 'm':
      do_mmap = true;
      break;
    case 'f':
      do_forever = true;
      break;
    case 'C':
      do_center = true;
      break;
    case 's':
      do_shuffle = true;
      break;
    case 'r':
      fprintf(stderr, "Instead of deprecated -r, use --led-rows=%s instead.\n",
              optarg);
      matrix_options.rows = atoi(optarg);
      break;
    case 'c':
      fprintf(stderr, "Instead of deprecated -c, use --led-chain=%s instead.\n",
              optarg);
      matrix_options.chain_length = atoi(optarg);
      break;
    case 'P':
      matrix_options.parallel = atoi(optarg);
      break;
    case 'L':
      fprintf(stderr, "-L is deprecated. Use\n\t--led-pixel-mapper=\"U-mapper\" --led-chain=4\ninstead.\n");
      return 1;
      break;
    case 'R':
      fprintf(stderr, "-R is deprecated. "
              "Use --led-pixel-mapper=\"Rotate:%s\" instead.\n", optarg);
      return 1;
      break;
    case 'O':
      stream_output = strdup(optarg);
      break;
    case 'V':
      img_param.vsync_multiple = atoi(optarg);
      if (img_param.vsync_multiple < 1) img_param.vsync_multiple = 1;
      break;
    case 'h':
    default:
      return usage(argv[0]);
    }

    // Starting from the current file, set all the remaining files to
    // the latest change.
    for (int i = optind; i < argc; ++i) {
      if ( strcmp(argv[i],"idle") == 0 )
      {
        idle_start = i - optind;
      }
      else
      {
        filename_params[argv[i]] = img_param;
      }
    }
  }

  const int filename_count = argc - optind;
  if (filename_count == 0) {
    fprintf(stderr, "Expected image filename.\n");
    return usage(argv[0]);
  }

  // Prepare matrix
  runtime_opt.do_gpio_init = (stream_output == NULL);
  matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL)
    return 1;

  printf( "REQUEST INPUTS: %lu\n", matrix->RequestInputs(1<<SPIN_SYNC) );

  offscreen_canvas = matrix->CreateFrameCanvas();

  printf("Size: %dx%d. Hardware gpio mapping: %s\n",
         matrix->width(), matrix->height(), matrix_options.hardware_mapping);

  // These parameters are needed once we do scrolling.
  const bool fill_width = false;
  const bool fill_height = false;

  // In case the output to stream is requested, set up the stream object.
  rgb_matrix::StreamIO *stream_io = NULL;
  rgb_matrix::StreamWriter *global_stream_writer = NULL;
  // if (stream_output) {
  //   int fd = open(stream_output, O_CREAT|O_WRONLY, 0644);
  //   if (fd < 0) {
  //     perror("Couldn't open output stream");
  //     return 1;
  //   }
  //   stream_io = new rgb_matrix::FileStreamIO(fd);
  //   global_stream_writer = new rgb_matrix::StreamWriter(stream_io);
  // }

  const tmillis_t start_load = GetTimeInMillis();
  fprintf(stderr, "Loading %d files...\n", argc - optind);
  // Preparing all the images beforehand as the Pi might be too slow to
  // be quickly switching between these. So preprocess.
  std::vector<FileInfo*> file_imgs;
  for (int imgarg = optind; imgarg < argc; ++imgarg) {
    const char *filename = argv[imgarg];
    FileInfo *file_info = NULL;

    std::string err_msg;
    std::vector<Magick::Image> image_sequence;
    if (LoadImageAndScale(filename, matrix->width(), matrix->height(),
                          fill_width, fill_height, &image_sequence, &err_msg)) {
      file_info = new FileInfo();
      file_info->params = filename_params[filename];
      file_info->content_stream = new rgb_matrix::MemStreamIO();
      file_info->is_multi_frame = image_sequence.size() > 1;
      rgb_matrix::StreamWriter out(file_info->content_stream);
      for (size_t i = 0; i < image_sequence.size(); ++i) {
        const Magick::Image &img = image_sequence[i];
        StoreInStream(img, 0, do_center, offscreen_canvas, &out);
      }         
    }
    if (file_info) {
      file_imgs.push_back(file_info);
    } else {
      fprintf(stderr, "%s skipped: Unable to open (%s)\n",
              filename, err_msg.c_str());
    }
  }

  // Some parameter sanity adjustments.
  if (file_imgs.empty()) {
    // e.g. if all files could not be interpreted as image.
    fprintf(stderr, "No image could be loaded.\n");
    return 1;
  } else if (file_imgs.size() == 1) {
    // Single image: show forever.
    file_imgs[0]->params.wait_ms = distant_future;
  } else {
    for (size_t i = 0; i < file_imgs.size(); ++i) {
      ImageParams &params = file_imgs[i]->params;
      // Forever animation ? Set to loop only once, otherwise that animation
      // would just run forever, stopping all the images after it.
      if (params.loops < 0 && params.anim_duration_ms == distant_future) {
        params.loops = 1;
      }
    }
  }

  fprintf(stderr, "Loading took %.3fs; now: Display.\n",
          (GetTimeInMillis() - start_load) / 1000.0);

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);
  signal(SIGUSR1, ResetHandler);


  /******** DISPLAY LOOP **********/

  bool sync, sync_last = 0, sync_loop = 0;
  int sync_frame = 0;
  // char read_buf [UART_BUF_SIZE];
  // size_t uart_buf_head = 0; // how many valid bytes currently in uart_buf

  pthread_create(&uart_thread, NULL, process_uart, NULL);

  fprintf(stderr, "PTHREAD : %lu\n",uart_thread);

  
  // do the actual displaying
  do {
    size_t i = 0;
    if(do_reset == true){
      sync_frame = 0;
      i = 0;
      do_reset = false;
    }

    DisplayAnimation(file_imgs[i], matrix, offscreen_canvas);

    sync = (matrix->AwaitInputChange(0))>>SPIN_SYNC & 0b1;
    if( sync_last != sync)
    {
      sync_last = sync;
      // only loop on idle
      if (sync == 0 && sync_loop == 1 )
      {
        i = sync_frame;
      }
    }
    if( ++i > file_imgs.size()-1 )
    {
      sync_loop = 1;
      sync_frame = idle_start;
      i = idle_start;
    }
  } while (do_forever && !interrupt_received);

  pthread_join(uart_thread, nullptr);

  if (interrupt_received) {
    fprintf(stderr, "Caught signal. Exiting.\n");
  }

  // Animation finished. Shut down the RGB matrix.
  matrix->Clear();
  delete matrix;

  // Leaking the FileInfos, but don't care at program end.
  return 0;
}
