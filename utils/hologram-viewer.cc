/*
* Drives a 64x64 HUB75 LED matrix (SLICE_ROWS, SLICE_COLS) to display continuous rotational slices of images (SLICE_COUNT) to create a volumetric 3D effect
*
* Monitors SPIN_SYNC gpio to measure rotation
* Starts zeromq server to receive LED controller commands (see: ./hologram-auto-controller.py)
* Reads .anim files from IMAGE_PATH for all slice data (see: ./image-to-rgb)
*
* $ make -C .
*
* see --help for led configuration
* common usage: hologram-viewer --led-rows=64 --led-cols=64 --led-pwm-dither-bits=2 --led-slowdown-gpio=2 --led-pwm-bits=3 --led-pwm-lsb-nanoseconds=50 --led-pixel-mapper="Rotate:270" --led-limit-refresh=1500
*/


#include <zmq.hpp>

#include "led-matrix.h"
#include "pixel-mapper.h"
#include "content-streamer.h"
#include "gpio.h"

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

#include <iostream>
#include <fstream>
#include <filesystem>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <pthread.h>

#include <Magick++.h>
#include <magick/image.h>

using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::StreamReader;

#define SPIN_SYNC 2 // gpio
#define UART_BUF_SIZE 8192 // 1024 should be enough but not by much

#define SLICE_ROWS 64
#define SLICE_COLS 64
#define SLICE_COUNT 100
#define SLICE_QUADRANT (SLICE_COUNT / 4)
#define SLICE_WRAP(slice) ((slice) % (SLICE_COUNT))

#define FRAME_TIME 100 // duration of each frame in milliseconds

/* GLOBALS */

typedef int64_t tmillis_t;
static const tmillis_t distant_future = (1LL<<40); // that is a while.
volatile uint32_t *timer_uS; // microsecond timer

// static char* anim_state = NULL;
// static int serial_fd;
// static uint8_t read_buf[UART_BUF_SIZE]; // rolling UART buffer
// static volatile size_t uart_buf_head = 0; // length of stored buffer
// static volatile bool read_busy = 0; // if process_uart is running

static pthread_t zmq_thread = 0;
volatile bool interrupt_received = false;

static rgb_matrix::RGBMatrix *matrix;
static rgb_matrix::FrameCanvas *offscreen_canvas;
static rgb_matrix::FrameCanvas *reader_canvas;

static std::string IMAGE_PATH = "/rpi-led-hologram/utils/anims/";

namespace fs = std::filesystem;

/* STRUCTS */

// used in StreamIO construction
struct Pixel
{
  Pixel() : r(0), g(0), b(0) {}
  unsigned char r;
  unsigned char g;
  unsigned char b;
  Pixel(unsigned char rn, unsigned char gn, unsigned char bn)
  {
    r = rn;
    g = gn;
    b = bn;
  }
};

// used in StreamIO construction
struct Slice
{
  Pixel pixels[SLICE_ROWS * SLICE_COLS]; //1D collapsed row*COLUMNS + col
  const Pixel GetPixel(size_t x, size_t y)
  {
    return pixels[ y * SLICE_COLS + x ];
  }
  void SetPixel(size_t x, size_t y,
                unsigned char r,
                unsigned char g,
                unsigned char b)
  {
    pixels[ y * SLICE_COLS + x ] = Pixel(r,g,b);
  }
};

struct SimpleFrame
{
  Slice slices[SLICE_COUNT];
};

struct MemFrame
{
  rgb_matrix::MemStreamIO slices[SLICE_COUNT];
};

// .anim file
struct AnimHeader
{
   char magic[9] = "HOLOGRAM"; // to recognize file (8+null)
   uint32_t frameCount = 0;
   uint32_t loopStart = 0; // frame to return to after last frame
};

struct Anim
{
  // std::vector<MemFrame> sequence;
  std::ifstream stream;
  std::streampos headHead;
  std::streampos loopHead;
  uint32_t frame = 0; // current frame
  uint32_t frameCount = 0;
  uint32_t loopStart = 0; // end anim -> loop/idle frame
};

static Anim *active_anim;
static std::string *next_anim_name, *active_anim_name;
static volatile bool do_change_anim = false; // flag to switch anim
static volatile bool do_next_frame = false; // flag to switch anim
static uint32_t d_us = 0;

static void InterruptHandler(int signo) {
  interrupt_received = true;
  if(zmq_thread > 0)
  {
    pthread_cancel(zmq_thread);
  }
}

static uint32_t sync_prev = 0;
static uint32_t rotation_angle = 0;
static int32_t rotation_delta = 256;
static int sync_level = 1;
static uint32_t tick_prev = 0;
static uint32_t rotation_history[8];

static int32_t rot_inc = 1; 

#define ROTATION_PRECISION 30
#define ROTATION_FULL (1<<ROTATION_PRECISION)
#define ROTATION_ZERO 286
#define ROTATION_HISTORY 8
#define ROTATION_HALF (1<<(ROTATION_PRECISION-1))
#define ROTATION_MASK ((1<<ROTATION_PRECISION)-1)

static uint32_t rotation_zero = ROTATION_FULL / 360 * ROTATION_ZERO;
// static bool rotation_stopped = true;
static uint32_t rotation_period_raw = 0;
static uint32_t rotation_period = 1<<26;
// static bool rotation_lock = true;
static int32_t rotation_drift = 0;
static volatile int rot_off = 0;


static tmillis_t GetTimeInMillis() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

// static void SleepMillis(tmillis_t milli_seconds) {
//   if (milli_seconds <= 0) return;
//   struct timespec ts;
//   ts.tv_sec = milli_seconds / 1000;
//   ts.tv_nsec = (milli_seconds % 1000) * 1000000;
//   nanosleep(&ts, NULL);
// }

int compare_ints(const void *a, const void *b) {
  return *((int*)a) - *((int*)b);
}

static uint32_t median_period() {
  static uint32_t sorted[ROTATION_HISTORY];
  memcpy(sorted, rotation_history, sizeof(sorted));
  qsort(sorted, ROTATION_HISTORY, sizeof(*sorted), compare_ints);

  return (sorted[3] + sorted[4])/2;
}

static uint32_t rotation_current_angle(void) {
  uint32_t tick_curr = rgb_matrix::GetMicrosecondCounter();
  uint32_t elapsed = tick_curr - sync_prev;
  
  static uint32_t current = 0;
  
  int sync = (matrix->AwaitInputChange(0))>>SPIN_SYNC & 0b1;
  if (sync != sync_level) {
    sync_level = sync;
    
    if(sync == 0) {
      sync_prev = tick_curr;
      rotation_period_raw = elapsed;
      if (elapsed > 10000) {
        if (++current >= ROTATION_HISTORY) {
            current = 0;
        }
        rotation_history[current] = elapsed;
        rotation_period = median_period();
        // std::cout << rotation_period << std::endl;

        rotation_delta = ROTATION_FULL / rotation_period;
        // if (rotation_lock) {
        //     int recentre = ((int32_t)((rotation_angle + (!sync * ROTATION_HALF)) & ROTATION_MASK) - ROTATION_HALF) >> 17;
        //     recentre = clamp(recentre, -rotation_delta / 16, rotation_delta / 16);
        //     rotation_delta -= recentre;
        // }
      }
    }
  }

  uint32_t dtick = (tick_curr - tick_prev);
  tick_prev = tick_curr;

  uint32_t delta = dtick * rotation_delta;

  rotation_angle = (rotation_angle + delta) & ROTATION_MASK;

  rotation_zero = (rotation_zero + ROTATION_FULL + (dtick * rotation_drift)) & ROTATION_MASK;

  return (rotation_angle + rotation_zero) & ROTATION_MASK;
}

/*
  StreamWriter appends image data to StreamIO (StreamWriter::Stream)
  reader.GetNext puts StreamIO on buffer canvas (need to rewind after)
  then SwapOnVSync

  StreamIO for each slice of each frame? or MemStreamIO::Read?

  streamio of each slice of each frame

  StreamIO slices[100]

  rgb_matrix::StreamReader reader( *(Anim.sequence + frame).slices[i] );
  reader.GetNext(buffer);
  matrix->SwapOnVSync(buffer);
  reader.Rewind();
  
*/

void ReadAnimFile(fs::path filepath, Anim &a)
{
  a.stream.open(filepath, std::ios::in | std::ios::binary );

  AnimHeader h;
  a.stream.read(reinterpret_cast<char*>(&h), sizeof(AnimHeader));
  a.headHead = a.stream.tellg();
  a.frameCount = h.frameCount;
  a.loopStart = h.loopStart;
  a.loopHead = a.headHead + static_cast<std::streampos>( sizeof(SimpleFrame) * a.loopStart );

  // // convert SimpleFrame to MemFrame for each frame
  // for(size_t i = 0; i < h.frameCount; i++)
  // {
  //   SimpleFrame frame;
  //   MemFrame memf;
  //   f.read(reinterpret_cast<char*>(&frame), sizeof(SimpleFrame));

  //   // store each slice in stream
  //   for(size_t k = 0; k < SLICE_COUNT; k++)
  //   {
  //     rgb_matrix::StreamWriter out(&memf.slices[k]);

  //     for (size_t y = 0; y < SLICE_ROWS; ++y) {
  //       for (size_t x = 0; x < SLICE_COLS; ++x) {
  //         const Pixel p = frame.slices[k].GetPixel(x, y);
  //         reader_canvas->SetPixel(x, y, p.r, p.g, p.b);
  //       }
  //     }

  //     out.Stream(*reader_canvas, 0); // out writes to StreamIO memf[k]
  //   }

  //   a.sequence.push_back(memf);
  // }
}

MemFrame GetNextFrame(Anim &a)
{
  // convert SimpleFrame to MemFrame for each frame
  SimpleFrame frame;
  MemFrame memf;
  a.frame++;
  if(a.frame >= a.frameCount)
  {
    a.frame = a.loopStart >= a.frameCount ? a.frameCount - 1 : a.loopStart;
    a.stream.clear();  // clear EOF flag
    a.stream.seekg(a.loopHead);
  }
  a.stream.read(reinterpret_cast<char*>(&frame), sizeof(SimpleFrame));

  // store each slice in stream
  for(size_t k = 0; k < SLICE_COUNT; k++)
  {
    rgb_matrix::StreamWriter out(&memf.slices[k]);

    for (size_t y = 0; y < SLICE_ROWS; ++y) {
      for (size_t x = 0; x < SLICE_COLS; ++x) {
        const Pixel p = frame.slices[k].GetPixel(x, y);
        reader_canvas->SetPixel(x, y, p.r, p.g, p.b);
      }
    }

    out.Stream(*reader_canvas, 0); // out writes to StreamIO memf[k]
  }

  return memf;
}

int RetrieveAnimList(std::map< std::string, Anim > &new_list)
{
  int i = 0;

  for( const auto & entry : fs::directory_iterator(IMAGE_PATH) )
  {
    if(entry.path().extension().string() == ".anim" )
    {
      new_list[entry.path().stem()] = Anim();
      ReadAnimFile( entry.path(), new_list[entry.path().stem()] );
      i++;
    }
  }
  return i;
}

void SwitchAnim(std::map< std::string, Anim > &AnimList)
{

  if( AnimList.count(*next_anim_name) == 0 )
  {
    RetrieveAnimList(AnimList); // reload anim list
    if( AnimList.count(*next_anim_name) == 0 ) return; // if still nothing
  }
  // active_anim->frame = 0;
  active_anim = &AnimList[*next_anim_name];
  active_anim->frame = 0;
  active_anim_name = next_anim_name;
}

void* zmq_loop (void* s)
{
  zmq::socket_t* socket = (zmq::socket_t*)s;
  std::string ok = "OK";
  std::string fail = "FAIL";

  while(!interrupt_received)
  {
    zmq::message_t request;
    
    // receive a request from client
    socket->recv(request, zmq::recv_flags::none);
    std::string r = request.to_string();
    std::cout << "REQ > " << r << std::endl;

    if( r[0] == '.' )
    {
      if( r == ".l" )
      {
        rot_off -= rot_inc;
      }
      if( r == ".r" )
      {
        rot_off += rot_inc;
      }
      if( r == ".n")
        do_next_frame = true;
    }
    else
    {
      next_anim_name = &r;
      do_change_anim = true;
    }
    
    // send the reply to the client
    socket->send(zmq::buffer(ok), zmq::send_flags::none);
  }
  return s;
}

int main(int argc, char *argv[])
{
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;

  /* ZEROMQ */
  zmq::context_t context(1);
  zmq::socket_t socket(context, zmq::socket_type::rep);

  // todo: zmq SERVER
  std::cout << "Binding ZMQ..." << std::endl;
  socket.bind("tcp://*:5555");

  // If started with 'sudo': make sure to drop privileges to same user
  // we started with, which is the most expected (and allows us to read
  // files as that user).
  runtime_opt.drop_priv_user = getenv("SUDO_UID");
  runtime_opt.drop_priv_group = getenv("SUDO_GID");

  rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                    &matrix_options, &runtime_opt);
  
  // runtime_opt.do_gpio_init = true;
  matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL)
    return 1;

  int opt;
  while ((opt = getopt(argc, argv, "r:")) != -1) {
    switch (opt) {
      case 'r': // directory
        rot_inc = atoi(optarg);
        break;
      default:
        break;
    }
  }
  
  printf( "REQUEST INPUTS: %lu\n", matrix->RequestInputs(1<<SPIN_SYNC) );

  offscreen_canvas = matrix->CreateFrameCanvas();
  reader_canvas = matrix->CreateFrameCanvas();
  
  printf("Size: %dx%d. Hardware gpio mapping: %s\n",
         matrix->width(), matrix->height(), matrix_options.hardware_mapping);
         
  const tmillis_t start_load = GetTimeInMillis();
  fprintf(stderr, "Loading files...\n");

  // store animations in AnimList
  std::map< std::string, Anim > AnimList;
  int anim_count = RetrieveAnimList(AnimList);
  if( anim_count == 0)
  {
    fprintf(stderr, "No .anim files found in %s\n", IMAGE_PATH.c_str());
    return 1;
  }

  fprintf(stderr, "Loading %d .anim files took %.3fs; now: Display.\n",
                  anim_count,
                  (GetTimeInMillis() - start_load) / 1000.0);

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  size_t i = 0;
  uint16_t prev_angle = 0;
  // uint16_t slice_angle = 0;

  std::cout << "Starting ZMQ thread..." << std::endl;
  pthread_create(&zmq_thread, NULL, zmq_loop, &socket);

  std::string startname = "idle";
  active_anim = &AnimList[startname];
  active_anim_name = &startname;

  tmillis_t last_time = GetTimeInMillis();

  MemFrame frame;

  std::cout << "Display begin" << std::endl;
  do {
    uint16_t slice_angle = SLICE_WRAP(((rotation_current_angle() >> (ROTATION_PRECISION - 10)) * SLICE_COUNT) >> 10);
    if( prev_angle > slice_angle ) // wrap-around (decrement)
      i += slice_angle + SLICE_COUNT - prev_angle;
    else // increment
      i += slice_angle - prev_angle;
    prev_angle = slice_angle;

    if( rot_off != 0 )
    {
      if( rot_off > 0 )
      {
        i++;
        rot_off--;
      }
      else // rot_off < 0
      {
        i--;
        rot_off++;
      }
      // i += rot_off;
      // rot_off = 0;
    }

    if( i >= SLICE_COUNT ) i = 0;
    if( i < 0 ) i = SLICE_COUNT - 1;

    if(do_change_anim)
    {
      SwitchAnim(AnimList);
      do_change_anim = false;
    }

    if( GetTimeInMillis() - last_time > FRAME_TIME )
    // if(do_next_frame)
    {
      last_time = GetTimeInMillis();
      frame = GetNextFrame(*active_anim);
      // active_anim->frame++;
      // if(active_anim->frame >= active_anim->frameCount)
      //   active_anim->frame = active_anim->loopStart >= active_anim->frameCount ? active_anim->frameCount - 1 : active_anim->loopStart;
      // do_next_frame = false;
    }

    // rgb_matrix::StreamReader reader(&active_anim->sequence.at(active_anim->frame).slices[i]);
    rgb_matrix::StreamReader reader(frame.slices[i]);
    while(!interrupt_received && reader.GetNext(offscreen_canvas, &d_us))
    {
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, 1);
    }
    reader.Rewind();
  } while (!interrupt_received);
}