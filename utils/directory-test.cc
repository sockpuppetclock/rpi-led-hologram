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
#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
   
  std::string path = "/home/dietpi/rpi-led-hologram/utils/images/";

  for( const auto & entry : fs::directory_iterator(path) )
  {
    if(entry.is_regular_file())
    {
      std::cout << entry.path() << std::endl;
    }
  }

   return 0;
}