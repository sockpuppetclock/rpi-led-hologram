/* * png -> binary (.anim) of r,g,b;r,g,b;r,g,b;...
* CLOCKWISE

* constructs a contiguous series of unsigned char rgb data
* per slice > per row in slice > per column in row > red, green, blue
* slices should be placed alphabetically in a folder

* $ sudo apt-get install libgraphicsmagick++-dev libwebp-dev

* usage: ./image-to-rgb -d <input folder> -o <output file> [-s <starting slice>]
*/ 

#include <iostream>
#include <fstream>
#include <filesystem>

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

struct Slice
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

struct SimpleFrame
{
  Slice slices[SLICE_COUNT];
};

// .anim file
struct AnimHeader
{
   char magic[9] = "HOLOGRAM"; // to recognize file (8+null)
   uint32_t frameCount = 0;
   uint32_t loopStart = 0; // frame to return to after last frame
};

// put sorted file list in given string vector
static void GetFileList(std::vector<std::string> *file_list, std::string folder)
{
  int files = 0;
  for( const auto & entry : fs::directory_iterator(folder) )
  {
    if(entry.path().extension().string() == ".png" )
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
                      Magick::Image *result,
                      std::string *err )
{
  std::cout << filename << std::endl;
  try
  {
    result->read(filename);
  }
  catch (std::exception& e)
  {
    if (e.what()) *err = e.what();
    return false;
  }
  return true;
}

void ImageToSlice(const Magick::Image *img, Slice *s)
{
  for (size_t y = 0; y < img->rows(); ++y) { // rows
    for (size_t x = 0; x < img->columns(); ++x) { // columns
      const Magick::Color &c = img->pixelColor(x, y);
      if (c.alphaQuantum() < 255) {
        // std::cout << std::to_string(ScaleQuantumToChar(c.redQuantum())) << std::endl;
        unsigned char r = ScaleQuantumToChar(c.redQuantum());
        unsigned char g = ScaleQuantumToChar(c.greenQuantum());
        unsigned char b = ScaleQuantumToChar(c.blueQuantum());
        s->SetPixel(x, y, r, g, b);
      }
    }
  }
}

int main(int argc, char *argv[]) {
  Magick::InitializeMagick(*argv);

  std::vector<SimpleFrame> anim;

  std::string folderpath;
  std::string outpath;
  int arg_loopstart = -1;

  int opt;
  while ((opt = getopt(argc, argv, "i:o:s:")) != -1) {
    switch (opt) {
      case 'i': // directory
        folderpath = optarg;
        break;
      case 'o':
        outpath = optarg;
        break;
      case 's':
        arg_loopstart = atoi(optarg);
        break;
      default:
        fprintf(stderr, "usage: %s -i <input folder> -o <output filename> [-s <loop frame>]\n", argv[0]);
        return 1;
        break;
    }
  }

  
  if( !fs::exists(folderpath) )
  {
    fprintf(stderr, "Input path \"%s\" does not exist\n", folderpath.c_str());
    return 1;
  }
  if( fs::exists(outpath) )
  {
    fprintf(stderr, "Output path \"%s\" already exists\n", folderpath.c_str());
    return 1;
  }
  if( !fs::is_directory(folderpath))
  {
    fprintf(stderr, "Path \"%s\" is not a folder\n", folderpath.c_str());
    return 1;
  }

  std::string anim_name = fs::path(folderpath).filename();
  
  std::vector<std::string> list;
  GetFileList(&list, folderpath); // put sorted file list in vector

  int frames = (int)list.size() / (int)SLICE_COUNT; // should truncate
  int count = 0;

  std::cout << frames << " frames" << std::endl;

  AnimHeader header;
  header.frameCount = frames;
  if( arg_loopstart == -1 )
    header.loopStart = frames - 1;
  else
    header.loopStart = arg_loopstart;

  // outpath.append(anim_name);
  // outpath.append(".anim");

  std::cout << "WRITING TO " << outpath << std::endl;

  std::ofstream f(outpath, std::ios::out | std::ios::binary | std::ios::app);
  f.write(reinterpret_cast<const char*>(&header), sizeof(AnimHeader));
  
  SimpleFrame *s;

  for(auto it = list.begin(); it != list.end() && count != (frames * SLICE_COUNT); ++it)
  {
    // size_t frame_num = count / (int)SLICE_COUNT;
    // if( anim.size() < frame_num + 1) // should only need +1 frame every 100 slices
    // {
    //   anim.emplace_back(SimpleFrame());
    // }

    if ( count % SLICE_COUNT == 0 )
    {
      if(count > 0)
      {
        f.write( reinterpret_cast<const char*>(s), sizeof(SimpleFrame) );
        delete s;
      }
      s = new SimpleFrame();
    }

    const char* filename = (const char*)it->c_str();
    Magick::Image img;
    std::string err;

    if( LoadImage( filename, &img, &err) )
    {
      // ImageToSlice( &img, &anim[frame_num].slices[count % SLICE_COUNT] );
      ImageToSlice( &img, &s->slices[count % SLICE_COUNT] );
    }
    else
    {
      fprintf(stderr, "%s error: %s", filename, err.c_str());
    }
    count++;
  }

  // for( auto it = anim.begin(); it != anim.end(); ++it )
  // {
  //   SimpleFrame *s = &(*it);
  //   f.write( reinterpret_cast<const char*>(s), sizeof(SimpleFrame) );
  // }
  f.close();

  return 0;
}