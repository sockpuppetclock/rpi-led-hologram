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

// https://blog.mbedded.ninja/programming/operating-systems/linux/linux-serial-ports-using-c-cpp/
#include <sys/ioctl.h>
#include <errno.h>
#include <termios.h>

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

/* GLOBALS */

typedef int64_t tmillis_t;
static const tmillis_t distant_future = (1LL<<40); // that is a while.
volatile uint32_t *timer_uS; // microsecond timer

static char* anim_state = NULL;
static int serial_fd;
static uint8_t read_buf[UART_BUF_SIZE]; // rolling UART buffer
static volatile size_t uart_buf_head = 0; // length of stored buffer
static volatile bool read_busy = 0; // if process_uart is running

static pthread_t zmq_thread = 0;
volatile bool interrupt_received = false;

static rgb_matrix::RGBMatrix *matrix;
static rgb_matrix::FrameCanvas *offscreen_canvas;
static rgb_matrix::FrameCanvas *reader_canvas;

static std::string IMAGE_PATH = "/hologram/utils/anim/";

namespace fs = std::filesystem;

/* STRUCTS */

// used in StreamIO construction
static struct Pixel
{
  Pixel(unsigned char, unsigned char, unsigned char);
  Pixel() : r(0), g(0), b(0) {}
  unsigned char r;
  unsigned char g;
  unsigned char b;
};

// used in StreamIO construction
static struct Slice
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

static struct SimpleFrame
{
  Slice slices[SLICE_COUNT];
};

static struct MemFrame
{
  rgb_matrix::MemStreamIO slices[SLICE_COUNT];
};

// .anim file
static struct AnimHeader
{
   char magic[9] = "HOLOGRAM"; // to recognize file (8+null)
   uint32_t frameCount = 0;
   uint32_t loopStart = 0; // frame to return to after last frame
}

static struct Anim
{
  std::vector<MemFrame> sequence;
  uint32_t frame = 0; // current frame
  uint32_t frameCount = 0;
  uint32_t loopStart = 0; // end anim -> loop/idle frame
};

static Anim *active_anim;
static volatile std::string *anim_next_name, anim_curr_name;
static uint32_t d_us = 0;

static void InterruptHandler(int signo) {
  interrupt_received = true;
  if(zmq_thread > 0)
  {
    pthread_cancel(zmq_thread);
  }
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
  std::ifstream f(filepath, std::ios::in | std::ios::binary );

  AnimHeader h;
  f.read(&h, sizeof(AnimHeader));
  a.frameCount = h.frameCount;
  a.loopStart = h.loopStart;

  // convert SimpleFrame to MemFrame for each frame
  for(size_t i = 0; i < h.frameCount; i++)
  {
    SimpleFrame frame;
    MemFrame memf;
    f.read(&frame, sizeof(SimpleFrame));

    // store each slice in stream
    for(size_t k = 0; k < SLICE_COUNT; k++)
    {
      rgb_matrix::StreamWriter out(memf[k]);

      for (size_t y = 0; y < SLICE_ROWS; ++y) {
        for (size_t x = 0; x < SLICE_COLS; ++x) {
          const Pixel p = frame.slices[k].GetPixel(x, y);
          reader_canvas->SetPixel(x, y, p.r, p.g, p.b);
        }
      }

      out.Stream(*reader_canvas, 0); // out writes to StreamIO memf[k]
    }

    a.sequence.push_back(memf);
  }
}

int RetrieveAnimList(std::map< std::string, Anim > &new_list)
{
  int i = 0;

  for( const auto & entry : fs::directory_iterator(IMAGE_PATH) )
  {
    if(entry.path().string().extension() == ".anim" )
    {
      new_list[entry.path().stem()] = Anim();
      ReadAnimFile( entry.path(), new_list[entry.path().stem()] );
      i++;
    }
  }
  return i;
}

int main(int argc, char *argv[])
{
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;

  /* ZEROMQ */
  zmq::context_t context (1);
  zmq::socket_t socket (context, zmq::socket_type::req);

  // todo: zmq SERVER
  pthread_create(&zmq_thread, NULL, zmq_loop, &socket);

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

  offscreen_canvas = matrix->CreateFrameCanvas();
  reader_canvas = matrix->CreateFrameCanvas();
  
  printf("Size: %dx%d. Hardware gpio mapping: %s\n",
         matrix->width(), matrix->height(), matrix_options.hardware_mapping);
         
  const tmillis_t start_load = GetTimeInMillis();
  fprintf(stderr, "Loading files...\n");
  
  std::map<std::string, Anim> AnimList;

  // store animations in AnimList
  int anim_count = RetrieveAnimList(AnimList);
  if( anim_count == 0)
  {
    fprintf(stderr, "No .anim files found in %s\n", IMAGE_PATH.c_str());
    return 1;
  }

  fprintf(stderr, "Loading %d files took %.3fs; now: Display.\n",
                  anim_count,
                  (GetTimeInMillis() - start_load) / 1000.0);

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  size_t i = 0;
  uint16_t prev_angle = 0;
  uint16_t slice_angle = 0;

  active_anim = &AnimList["miku"];
  anim_curr_name = 

  do {
    uint16_t slice_angle = SLICE_WRAP(((rotation_current_angle() >> (ROTATION_PRECISION - 10)) * SLICE_COUNT) >> 10);
    if( prev_angle > slice_angle ) // wrap-around
      i += slice_angle + SLICE_COUNT - prev_angle;
    else // increment
      i += slice_angle - prev_angle;
    prev_angle = slice_angle;

    rgb_matrix::StreamReader reader(active_anim->sequence.at(active_anim->frame).slices[i]);
    while(!interrupt_received && reader.GetNext(offscreen_canvas, &d_us))
    {
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, 1);
    }
    reader.Rewind();
  }
}

void SwitchAnim(volatile std::string )