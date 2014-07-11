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
  char tcpnodelay = 0, asyncRequests = 0, wait = 0, *logfilename = NULL;
  int clientfd;
  ssize_t n = 1;
  size_t i = 0, delay, lastRequest, delta, requestCount = 0, requestsPending = 0, bytesRead = 0, bytesWritten = 0;
  socklen_t addrlen;
  uint64_t readStart = 0, readEnd, writeStart = 0, writeEnd;
  char *host, *port, addrstr[INET6_ADDRSTRLEN];
  struct setup_header setupBuffer;
  struct request_header * requestBuffer;
  struct response_header * responseBuffer;
  struct timeval timeout;
  struct sockaddr_in addr;
  fd_set rfds, wfds;
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
      tcpnodelay = 1;
      break;
    case 'w':
      wait = 1;
      break;
    case 'a':
      asyncRequests = 1;
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

  if (tcpnodelay) {
    if(setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, (char*)&tcpnodelay, sizeof(int)) == -1)
    {
      fprintf(stderr, "Error setting no delay - continuing\n");
    } else {
      LOG(logfile, "client: set TCP_NODELAY on socket\n");
    }
  }

  write(clientfd, &setupBuffer, sizeof(setupBuffer));

  lastRequest = 0;
  while (requestCount < setupBuffer.requests || requestsPending > 0) {
    FD_ZERO(&rfds);

    if (requestsPending > 0) {
      FD_SET(clientfd, &rfds);
    }

    delta = microseconds() - lastRequest;
    FD_ZERO(&wfds);
    if ((asyncRequests || !requestsPending) && delta > delay) {
      FD_SET(clientfd, &wfds);
    }
    if (delta > delay) {
      timeout.tv_sec = 0;
      timeout.tv_usec = 0;
    } else {
      timeout.tv_sec = (delay - delta) / 1000000L;
      timeout.tv_usec = (delay - delta) % 1000000L;
    }


    LOG(logfile, "%lu client: waiting for input, %lu requests pending\n", microseconds(), requestsPending);

    select(clientfd+1, &rfds, &wfds, NULL, (!asyncRequests && requestsPending) ? NULL : &timeout);

    if (FD_ISSET(clientfd, &rfds)) {
      LOG(logfile, "%lu client: recieving up to %lu bytes\n", microseconds(), setupBuffer.response_size - bytesRead);
      if (!bytesRead) {
        readStart = microseconds();
      }
      n = read(clientfd, ((char*)responseBuffer) + bytesRead, setupBuffer.response_size - bytesRead);
      if (n < 0)
      {
        perror("read: ");
        fprintf(stderr, "Error reading from socket\n");
        break;
      }
      if (n == 0) {
        LOG(logfile, "%lu client: connection closed\n", microseconds());
        break;
      }
      readEnd = microseconds();

      LOG(logfile, "%lu client: recieved %lu bytes\n", microseconds(), n);
      bytesRead += n;
      if (bytesRead == setupBuffer.response_size) {
        LOG(logfile, "%lu client: request %lu response write ended at %lu\n", microseconds(), responseBuffer->prev_seq, responseBuffer->prev_write_end);
        LOG(logfile, "%lu client: request %lu request read started at %lu\n", microseconds(), responseBuffer->seq, responseBuffer->read_start);
        LOG(logfile, "%lu client: request %lu request read ended at %lu\n", microseconds(), responseBuffer->seq, responseBuffer->read_end);
        LOG(logfile, "%lu client: request %lu response read started at %lu\n", microseconds(), responseBuffer->seq, readStart);
        LOG(logfile, "%lu client: request %lu response read ended at %lu\n", microseconds(), responseBuffer->seq, readEnd);
        LOG(logfile, "%lu client: request %lu response write started at %lu\n", microseconds(), responseBuffer->seq, responseBuffer->write_start);
        --requestsPending;
        bytesRead = 0;
      }
    }

    if (FD_ISSET(clientfd, &wfds)) {
      LOG(logfile, "%lu client: writing bytes\n", microseconds());
      requestBuffer->seq = requestCount + 1;
      if (!bytesWritten) {
        writeStart = microseconds();
      }
      n = write(clientfd, ((char*)requestBuffer) + bytesWritten, setupBuffer.request_size - bytesWritten);
      if (n < 0) {
        perror("write: ");
        fprintf(stderr, "Error writing to socket\n");
        break;
      }
      if (n == 0) {
        LOG(logfile, "%lu client: connection closed\n", microseconds());
        break;
      }
      writeEnd = microseconds();

      LOG(logfile, "%lu client: sent %lu bytes\n", microseconds(), setupBuffer.request_size);
      bytesWritten += n;
      if (bytesWritten == setupBuffer.request_size) {
        ++requestCount;
        ++requestsPending;
        lastRequest = microseconds();
        bytesWritten = 0;

        LOG(logfile, "%lu client: request %lu request write started at %lu\n", microseconds(), requestCount, writeStart);
        LOG(logfile, "%lu client: request %lu request write ended at %lu\n", microseconds(), requestCount, writeEnd);
      }
    }
  }
  close(clientfd);
  return 0;
}

