#include "content-streamer.h"
#include <fstream>

#define SLICE_ROWS 64
#define SLICE_COLS 64
#define SLICE_COUNT 100

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