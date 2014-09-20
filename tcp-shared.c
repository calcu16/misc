#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netdb.h>
#include "tcp-shared.h"

size_t request_find_slot(struct request* requests, size_t seq, size_t last, size_t size) {
  for (; requests[last].seq != seq; last = (last + 1) % size) ;
  return last;
}

