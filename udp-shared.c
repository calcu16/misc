#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netdb.h>
#include "traffic-shared.h"
#include "udp-shared.h"

uint64_t rcvd_microseconds(int fd)
{
  struct timeval tv;
  memset(&tv, 0, sizeof(tv));
  if (ioctl(fd, SIOCGSTAMP, &tv) == -1) {
    perror("ioctl");
  }
  return tv.tv_sec * (uint64_t) 1000000 + tv.tv_usec;
}

