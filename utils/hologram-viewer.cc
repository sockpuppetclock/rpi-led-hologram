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
#include "hologram-viewer.h"

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
#include <thread>
#include <atomic>

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

#define SLICE_ROWS 64
#define SLICE_COLS 64
#define SLICE_COUNT 100
#define SLICE_QUADRANT (SLICE_COUNT / 4)
#define SLICE_WRAP(slice) ((slice) % (SLICE_COUNT))

#define FRAME_TIME 100 // duration of each frame in milliseconds
#define QUEUE_SLOTS 30 // max frames to queue ahead

/* GLOBALS */

typedef int64_t tmillis_t;

static std::thread zmq_thread;
volatile bool interrupt_received = false;

static rgb_matrix::RGBMatrix *matrix;
static rgb_matrix::FrameCanvas *offscreen_canvas;
static rgb_matrix::FrameCanvas *reader_canvas;

static std::string IMAGE_PATH = "/rpi-led-hologram/utils/anims/";

namespace fs = std::filesystem;

// rolling FIFO queue
template<typename T>
class SPSCQueue {
public:
    SPSCQueue(size_t capacity) : cap_(capacity), buf_(capacity) {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    // non-blocking push, returns false if full
    bool push(const T &v) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % cap_;
        if (next == head_.load(std::memory_order_acquire)) return false; // full
        buf_[tail] = v;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // non-blocking pop, returns false if empty
    bool pop(T &out) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false; // empty
        out = buf_[head];
        head_.store((head + 1) % cap_, std::memory_order_release);
        return true;
    }

    bool isfull()
    {
      size_t tail = tail_.load(std::memory_order_relaxed);
      size_t next = (tail + 1) % cap_;
      if (next == head_.load(std::memory_order_acquire)) return true;
      return false;
    }

    // clean out queue immediately
    void clear() {
      auto t = tail_.load(std::memory_order_acquire);
      head_.store(t, std::memory_order_release);
    }

private:
    const size_t cap_;
    std::vector<T> buf_;
    std::atomic<size_t> head_, tail_;
};

constexpr static size_t HEADER_SIZE = sizeof(AnimHeader);
constexpr static size_t FRAME_SIZE = sizeof(SimpleFrame);

static std::string *next_anim_name, *active_anim_name;
static volatile bool do_change_anim = false; // flag to switch anim
static volatile bool do_next_frame = false; // flag to switch anim
static uint32_t d_us = 0;

static void InterruptHandler(int signo) {
  interrupt_received = true;
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

// create ifstream for .anim and get header data
void ReadAnimFile(fs::path filepath, Anim &a)
{
  a.stream.open(filepath, std::ios::in | std::ios::binary );

  AnimHeader h;
  a.stream.read(reinterpret_cast<char*>(&h), HEADER_SIZE);
  a.headHead = a.stream.tellg();
  a.frameCount = h.frameCount;
  a.loopStart = h.loopStart > h.frameCount ? h.frameCount : h.loopStart;
  a.loopHead = a.headHead + static_cast<std::streampos>( FRAME_SIZE * a.loopStart );

  // // convert SimpleFrame to MemFrame for each frame
  // for(size_t i = 0; i < h.frameCount; i++)
  // {
  //   SimpleFrame frame;
  //   MemFrame memf;
  //   f.read(reinterpret_cast<char*>(&frame), FRAME_SIZE);

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

MemFrame EmptyFrame()
{
  MemFrame memf;
  for(size_t k = 0; k < SLICE_COUNT; k++)
  {
    rgb_matrix::StreamWriter out(&memf.slices[k]);
    for (size_t y = 0; y < SLICE_ROWS; ++y) {
      for (size_t x = 0; x < SLICE_COLS; ++x) {
        reader_canvas->SetPixel(x, y, 0, 0, 0);
      }
    }
    out.Stream(*reader_canvas, 0); // out writes to StreamIO memf[k]
  }
  return memf;
}

// MemFrame GetNextFrame(Anim &a)
// {
//   // convert SimpleFrame to MemFrame for each frame
//   SimpleFrame frame;
//   MemFrame memf;
//   a.frame++;
//   if(a.frame >= a.frameCount)
//   {
//     a.frame = a.loopStart >= a.frameCount ? a.frameCount - 1 : a.loopStart;
//     a.stream.clear();  // clear EOF flag
//     a.stream.seekg(a.loopHead);
//   }
//   a.stream.read(reinterpret_cast<char*>(&frame), sizeof(SimpleFrame));

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

//   return memf;
// }

int RetrieveAnimList(std::map< std::string, Anim > &new_list)
{
  int i = 0;

  for( const auto & entry : fs::directory_iterator(IMAGE_PATH) )
  {
    if(entry.path().extension().string() == ".anim" )
    {
      // todo: check if already exists?
      new_list[entry.path().stem()] = Anim();
      ReadAnimFile( entry.path(), new_list[entry.path().stem()] );
      i++;
    }
  }
  return i;
}

// void SwitchAnim(std::map< std::string, Anim > &AnimList)
// {

//   if( AnimList.count(*next_anim_name) == 0 )
//   {
//     RetrieveAnimList(AnimList); // reload anim list
//     if( AnimList.count(*next_anim_name) == 0 ) return; // if still nothing
//   }
//   // active_anim->frame = 0;
//   active_anim = &AnimList[*next_anim_name];
//   active_anim->frame = 0;
//   active_anim_name = next_anim_name;
// }

void zmq_loop (void* s)
{
  zmq::socket_t* socket = (zmq::socket_t*)s;
  std::string ok = "OK";
  std::string fail = "FAIL";

  // avoid blocking indefinitely
  socket->set(zmq::sockopt::rcvtimeo, 300); // 100ms timeout

  while(!interrupt_received)
  {
    zmq::message_t request;
    
    // receive a request from client
    if(socket->recv(request, zmq::recv_flags::none))
    {
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
    std::this_thread::yield();
  }
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
  zmq_thread = std::thread(zmq_loop, &socket);

  SPSCQueue<MemFrame> readyQueue(QUEUE_SLOTS + 1);

  std::string startname = "idle";
  Anim* active_anim(&AnimList[startname]);
  active_anim_name = &startname;

  tmillis_t last_time = GetTimeInMillis();

  MemFrame active_frame = EmptyFrame();

  // starts producer thread
  std::thread producer([&](){
    while(!interrupt_received)
    {
      if(do_change_anim)
      {
        if( AnimList.count(*next_anim_name) == 0 )
        {
          RetrieveAnimList(AnimList); // reload anim list        
        }
        if( AnimList.count(*next_anim_name) != 0 )
        {
          active_anim = &AnimList[*next_anim_name];
          active_anim->frame = 0;
          active_anim_name = next_anim_name;
          readyQueue.clear();
        }
        do_change_anim = false;
      }

      if(readyQueue.isfull())
      {
        std::this_thread::yield();
        continue;
      }
    
      // convert SimpleFrame to MemFrame for each frame
      SimpleFrame data;
      MemFrame next_frame;
      active_anim->frame++;
      if(active_anim->frame >= active_anim->frameCount)
      {
        active_anim->frame = active_anim->loopStart >= active_anim->frameCount ? active_anim->frameCount - 1 : active_anim->loopStart;
        active_anim->stream.clear();  // clear EOF flag
        active_anim->stream.seekg(active_anim->loopHead);
      }
      active_anim->stream.read(reinterpret_cast<char*>(&data), FRAME_SIZE);

      // store each slice in stream
      for(size_t k = 0; k < SLICE_COUNT; k++)
      {
        rgb_matrix::StreamWriter out(&next_frame.slices[k]);

        for (size_t y = 0; y < SLICE_ROWS; ++y) {
          for (size_t x = 0; x < SLICE_COLS; ++x) {
            const Pixel p = data.slices[k].GetPixel(x, y);
            reader_canvas->SetPixel(x, y, p.r, p.g, p.b);
          }
        }

        out.Stream(*reader_canvas, 0); // out writes to StreamIO memf[k]
      }

      readyQueue.push(next_frame);
      std::this_thread::yield();
    }
  });

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
        if( i == 0 ) i = SLICE_COUNT - 1;
        else i--;
        rot_off++;
      }
      // i += rot_off;
      // rot_off = 0;
    }

    if( i >= SLICE_COUNT ) i = 0;

    if( GetTimeInMillis() - last_time > FRAME_TIME )
    {
      last_time = GetTimeInMillis();
      readyQueue.pop(active_frame);
    }

    rgb_matrix::StreamReader reader(&active_frame.slices[i]);
    while(!interrupt_received && reader.GetNext(offscreen_canvas, &d_us))
    {
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, 1);
    }
    reader.Rewind();
  } while (!interrupt_received);

  std::cout << "Ending display..." << std::endl;

  // shutdown
  if(zmq_thread.joinable()) zmq_thread.join();
  if(producer.joinable()) producer.join();
  socket.close();
  for (auto& a : AnimList) {
      a.second.stream.close();
  }

  return 0;
}