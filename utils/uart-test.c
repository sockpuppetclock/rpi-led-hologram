// SOURCE: https://blog.mbedded.ninja/programming/operating-systems/linux/linux-serial-ports-using-c-cpp/
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifndef CRTSCTS
#define CRTSCTS  020000000000
#endif

int beginUART() {
  int fd = open("/dev/ttyS0", O_RDONLY);
  struct termios tty;
  if(tcgetattr(fd, &tty) != 0) {
      printf("Error %i from tcgetattr: %s\n", errno, strerror(errno));
      return 1;
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
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
      printf("Error %i from tcsetattr: %s\n", errno, strerror(errno));
      return 1;
  }

  // while (1)
  // {
    // char read_buf [256];
    // int bytes;
    // ioctl(fd, FIONREAD, &bytes);
    // if( bytes > 0 )
    // {
    //     memset(&read_buf, '\0', sizeof(read_buf));
    //     int num_bytes = read(fd, &read_buf, sizeof(read_buf));
    //     if (num_bytes < 0)
    //     {
    //         printf("Error reading: %s", strerror(errno));
    //         break;
    //     }
    //     if(num_bytes > 0)
    //     {
    //         printf("%s\n", read_buf);
    //     }
    // }
  // }
  // close(fd);
  return 0;
}