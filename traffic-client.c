#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include "traffic-shared.h"

#define LOG(log, ...)            \
  do {                           \
    printf(__VA_ARGS__);         \
    if (log != NULL) {           \
      fprintf(log, __VA_ARGS__); \
    }                            \
  } while(0)

/* main driver function */
int main(int argc, char **argv)
{
  char tcp_nodelay = 0, async_requests = 0, wait = 0, *logfilename = NULL;
  int clientfd;
  size_t n = 1, i = 0, delay, lastRequest, delta, requestCount = 0, requests_pending = 0, bytesRead = 0;
  socklen_t addrlen;
  uint64_t read_start = 0;
  char *host, *port, addrstr[INET6_ADDRSTRLEN];
  struct setup_header setupBuffer;
  struct request_header * requestBuffer;
  struct response_header * responseBuffer;
  struct timeval timeout;
  struct sockaddr_in addr;
  fd_set rfds;
  FILE * logfile = NULL;

  while (argc >= 3 && argv[1][0] == '-') {
    switch(argv[1][++i])
    {
    case 0:
      argc -= n;
      argv += n;
      i = 0;
      n = 1;
      break;
    case 'n':
      tcp_nodelay = 1;
      break;
    case 'w':
      wait = 1;
      break;
    case 'a':
      async_requests = 1;
      break;
    case 'l':
      logfilename = argv[++n];
      break;
    default:
      fprintf(stderr, "Unrecognized option '%c'\n", argv[1][1]);
      return 1;
    }
  }

  if (logfilename) {
    logfile = fopen(logfilename, "a");
  }

  if (argc <= 6) {
    fprintf(stderr, "Usage : traffic-client [-anw] [-l LOGFILE] HOST PORT REQUESTS REQUEST_SIZE RESPONSE_SIZE MIN_REQUEST_DELAY\n");
    return 1;
  }

  host = argv[1];
  port = argv[2];
  setupBuffer.requests = atol(argv[3]);
  setupBuffer.request_size = atol(argv[4]);
  setupBuffer.response_size = atol(argv[5]);
  delay = atol(argv[6]);

  if (setupBuffer.request_size < sizeof(struct request_header)) {
    fprintf(stderr, "REQUEST_SIZE must be at least %lu\n", sizeof(struct request_header));
    return 1;
  }

  if (setupBuffer.response_size < sizeof(struct response_header)) {
    fprintf(stderr, "RESPONSE_SIZE must be at least %lu\n", sizeof(struct response_header));
    return 1;
  }

  requestBuffer = malloc(setupBuffer.request_size);
  responseBuffer = malloc(setupBuffer.response_size);

  /* looks up server and connects */
  if((clientfd = open_socketfd(host, port, AI_V4MAPPED, &connect)) < 0)
  {
    fprintf(stderr, "Error connecting to server %d\n", clientfd);
    return 1;
  }

  LOG(logfile, "client: connected\n");

  addrlen = sizeof(addr);
  if (getsockname(clientfd, (struct sockaddr*)&addr, (socklen_t*)&addrlen)) {
    fprintf(stderr, "Error getting socket name\n");
  } else {
    inet_ntop(AF_INET, &addr.sin_addr, addrstr, sizeof(addrstr));
    LOG(logfile, "Connecting from %s:%d\n", addrstr, ntohs(addr.sin_port));
  }

  addrlen = sizeof(addr);
  if (getpeername(clientfd, (struct sockaddr*)&addr, (socklen_t*)&addrlen)) {
    fprintf(stderr, "Error getting socket name\n");
  } else {
    inet_ntop(AF_INET, &addr.sin_addr, addrstr, sizeof(addrstr));
    LOG(logfile, "Connecting to %s:%d\n", addrstr, ntohs(addr.sin_port));
  }

  if (wait) {
    getchar();
  }

  if (tcp_nodelay) {
    if(setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, (char*)&tcp_nodelay, sizeof(int)) == -1)
    {
      fprintf(stderr, "Error setting no delay - continuing\n");
    } else {
      LOG(logfile, "client: set TCP_NODELAY on socket\n");
    }
  }

  write(clientfd, &setupBuffer, sizeof(setupBuffer));

  lastRequest = 0;
  while (requestCount < setupBuffer.requests || requests_pending > 0) {
    FD_ZERO(&rfds);
    FD_SET(clientfd, &rfds);

    delta = microseconds() - lastRequest;
    if (delta > delay) {
      timeout.tv_sec = 0;
      timeout.tv_usec = 0;
    } else {
      timeout.tv_sec = (delay - delta) / 1000000L;
      timeout.tv_usec = (delay - delta) % 1000000L;
    }

    LOG(logfile, "%lu client: waiting for input %lu requests pending\n", microseconds(), requests_pending);
    select(clientfd+1, &rfds, NULL, NULL, (!async_requests && requests_pending) ? NULL : &timeout);

    if (FD_ISSET(clientfd, &rfds)) {
      LOG(logfile, "%lu client: recieving bytes\n", microseconds());
      if (!bytesRead) {
        read_start = microseconds();
      }
      n = read(clientfd, responseBuffer + bytesRead, setupBuffer.response_size - bytesRead);
      if (n < 0)
      {
        fprintf(stderr, "Error reading from socket\n");
        break;
      }
      if (n == 0) {
        LOG(logfile, "%lu client: connection closed\n", microseconds());
        break;
      }
      LOG(logfile, "%lu client: recieved %lu bytes\n", microseconds(), n);
      bytesRead += n;
      if (bytesRead == setupBuffer.response_size) {
        LOG(logfile, "%lu client: request %lu response read ended at %lu\n", microseconds(), responseBuffer->seq, microseconds());
        LOG(logfile, "%lu client: request %lu response read started at %lu\n", microseconds(), responseBuffer->seq, read_start);
        LOG(logfile, "%lu client: request %lu response write ended at %lu\n", microseconds(), responseBuffer->prev_seq, responseBuffer->prev_write_end);
        LOG(logfile, "%lu client: request %lu request read started at %lu\n", microseconds(), responseBuffer->seq, responseBuffer->read_start);
        LOG(logfile, "%lu client: request %lu request read ended at %lu\n", microseconds(), responseBuffer->seq, responseBuffer->read_end);
        LOG(logfile, "%lu client: request %lu response write started at %lu\n", microseconds(), responseBuffer->seq, responseBuffer->write_start);
        --requests_pending;
        bytesRead = 0;
      }
    } else if (requestCount < setupBuffer.requests) {
      ++requestCount;
      requestBuffer->seq = requestCount;
      LOG(logfile, "%lu client: sending %lu bytes\n", microseconds(), setupBuffer.request_size);
      LOG(logfile, "%lu client: request %lu request write started at %lu\n", microseconds(), requestCount, microseconds());
      n = write(clientfd, requestBuffer, setupBuffer.request_size);
      if (n == 0) {
        LOG(logfile, "%lu client: connection closed\n", microseconds());
        break;
      }
      LOG(logfile, "%lu client: request %lu request write ended at %lu\n", microseconds(), requestCount, microseconds());
      lastRequest = microseconds();
      ++requests_pending;
      LOG(logfile, "%lu client: sent %lu bytes\n", lastRequest, setupBuffer.request_size);
    }
  }
  close(clientfd);
  return 0;
}

