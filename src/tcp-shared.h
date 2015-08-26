#ifndef TCP_SHARED_H
#define TCP_SHARED_H
#include "traffic-shared.h"
struct setup_header {
  uint64_t requests, request_size, response_size, simul;
};

struct request_header {
  uint64_t seq, index;
};

struct response_header {
  uint64_t prev_seq, prev_write_end, prev_index, seq, index, rcvd, read_start, read_end, write_start;
};


struct request {
  size_t seq, index;
  uint64_t request_write_start, request_write_end, request_rcvd, request_read_start, request_read_end;
  uint64_t response_write_start, response_write_end, response_rcvd, response_read_start, response_read_end;
};

size_t request_find_slot(struct request* requests, size_t seq, size_t expected, size_t size);
#endif/*TCP_SHARED_H*/

