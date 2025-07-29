// png -> binary (.frame) of r,g,b;r,g,b;r,g,b;...
// 
// CLOCKWISE

// constructs a contiguous series of unsigned char rgb data
// per slice > per row in slice > per column in row > red, green, blue

// $ sudo apt-get install libgraphicsmagick++-dev libwebp-dev

#include <iostream>
#include <fstream>
#include <filesystem>

#include <Magick++.h>
#include <magick/image.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#define SLICE_ROWS 64
#define SLICE_COLS 64
#define SLICE_COUNT 100

namespace fs = std::filesystem;

static struct Pixel
{
  Pixel(unsigned char, unsigned char, unsigned char);
  Pixel() : r(0), g(0), b(0) {}
  unsigned char r;
  unsigned char g;
  unsigned char b;
};

static struct Slice
{
  Pixel pixels[SLICE_ROWS * SLICE_COLS]; //1D collapsed row*COLUMNS + col
  Pixel GetPixel(size_t x, size_t y)
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

// .anim file
static struct AnimHeader
{
   char magic[9] = "HOLOGRAM"; // to recognize file (8+null)
   uint32_t frameCount = 0;
   uint32_t loopStart = 0; // frame to return to after last frame
}

// put sorted file list in given string vector
static void GetFileList(std::vector<std::string> *file_list, std::string folder)
{
  int files = 0;
  for( const auto & entry : fs::directory_iterator(folder) )
  {
    if(entry.path().string().extension() == ".png" )
    {
      files++;
      file_list->push_back(entry.path().string());
    }
  }
  std::sort(file_list->begin(), file_list->end());

  std::cout << files << " files found" << std::endl;
}


// read png into Magick::Image result
static bool LoadImage(const char *filename,
                      Magick::Image &result,
                      std::string *err )
{
  try
  {
    result.read(filename);
  }
  catch (std::exception& e)
  {
    if (e.what()) *err = e.what();
    return false
  }
  return true;
}

void ImageToSlice(const Magick::Image &img, Slice *s)
{
  for (size_t y = 0; y < SLICE_ROWS; ++y) { // rows
    for (size_t x = 0; x < SLICE_COLS; ++x) { // columns
      const Magick::Color &c = img.pixelColor(x, y);
      if (c.alphaQuantum() < 255) {
        s->SetPixel(x, y, ScaleQuantumToChar(c.redQuantum()),
                          ScaleQuantumToChar(c.greenQuantum()),
                          ScaleQuantumToChar(c.blueQuantum()));
      }
    }
  }
}

int main(int argc, char *argv[]) {
  Magick::InitializeMagick(*argv);

  std::vector<SimpleFrame> anim;

  std::string folderpath;
  int arg_loopstart = 0;

  int opt;
  while ((opt = getopt(argc, argv, "d:s:")) != -1) {
    switch (opt) {
      case 'd': // directory
        folderpath = optarg;
        break;
      case 's':
        arg_loopstart = atoi(optarg);
      default:
        fprintf(stderr, "usage: %s <directory>", argv[0]);
        return 1;
        break;
    }
  }

  
  if( !fs::exists(folderpath) )
  {
    fprintf(stderr, "Path \"%s\" does not exist", folderpath.c_str());
    return 1;
  }
  if( !fs::is_directory(folderpath))
  {
    fprintf(stderr, "Path \"%s\" is not a folder", folderpath.c_str());
    return 1;
  }

  std::string anim_name = fs::path(folderpath).filename();
  
  std::vector<std::string> list;
  GetFileList(list, folderpath); // put sorted file list in vector

  int frames = (int)list.size() / (int)SLICE_COUNT; // should truncate
  int count = 0;
  auto it = list.begin();
  while(it != list.end() && count != frames)
  {
    int frame_num = count / (int)SLICE_COUNT + 1;
    if( anim.size() < frame_num ) // should only need +1 frame every 100 slices
    {
      anim.emplace_back(SimpleFrame());
    }

    const char* filename = (const char*)it->c_str();
    Magick::Image img;
    std::string err;

    if( LoadImage( filename, &img, &err) )
    {
      ImageToSlice( &img, &anim[frame_num].slices[count % SLICE_COUNT] );
    }
    else
    {
      fprintf(stderr, "%s error: %s", filename, err.c_str());
    }
    count++;
    ++it;
  }

  AnimHeader header;
  header.frameCount = frames;
  header.loopStart = arg_loopstart;

  anim_name.append(".anim");

  std::ofstream f(anim_name, std::ios::out | std::ios::binary | std::ios::app);
  f.write(header, sizeof(AnimHeader));
  for( auto it = anim.begin(); it != anim.end(); ++it )
  {
    f.write( reinterpret_cast<char*>(*it), sizeof(SimpleFrame) );
  }
  f.close();

  return 0;
}