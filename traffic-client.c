#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include "traffic-shared.h"

struct options {
  int argc;
  char **argv;

  size_t delay, simul;
  char *logfilename;
  char tcpnodelay, *log_level, wait;
};


#define TYPE "client"

#define SWITCH_TWO(a,b) switch(!!(a) << 1 | !!(b))

static const char usage[] =
  "usage: %s [-hnvw] [-d DELAY] [-s NUM_SIMUL] [-l LOGFILE] [-d DELAY] HOST PORT REQUESTS REQUEST_SIZE RESPONSE_SIZE\n"
  "  -d          : Delay between consecutive requests\n"
  "  -h          : Print help and exit\n"
  "  -l=/dev/null: Duplicate all statements to a logfile\n"
  "  -n          : Use tcp no delay on outgoing connections\n"
  "  -s=1        : Allow for NUM_SIMUL requests at the same time\n"
  "  -v          : Verbose printing\n"
  "  -w          : Wait for input from stdin after connecting but before sending the normal requests\n"
  ;

static int optparse(struct options *options)
{
  size_t i = 0;
  size_t n = 1;

  while (options->argc >= 2 && options->argv[0][0] == '-') {
    switch(options->argv[0][++i]) {
    case 'd': options->delay = atoll(&options->argv[0][n++]); break;
    case 's': options->simul = atoll(&options->argv[0][n++]); break;
    case 'l': options->logfilename = &options->argv[0][n++]; break;
    case 'n': options->tcpnodelay = 1; break;
    case 'w': options->wait = 1; break;
    case 'v': options->log_level[0] = LOG_LEVEL_V; break;
    case 'q': options->log_level[0] = LOG_LEVEL_Q; break;
    case 'h': return 1;
    case '-':
      options->argc -= n;
      options->argv += n;
      return 0;
    case 0:
      options->argc -= n;
      options->argv += n;
      i = 0;
      n = 1;
      break;
    default:
      fprintf(stderr, "Unrecognized option '%c' (%d)\n", options->argv[0][i], (int)options->argv[0][i]);
      return 2;
    }
  }
  return 0;
}

/* main driver function */
int main(int argc, char **argv)
{
  char *progname = argv[0], *host, *port, addrstr[INET6_ADDRSTRLEN];
  int clientfd, error;
  ssize_t n = 1;
  size_t iw = 0, ir = 0, delta, requestCount = 0, responseCount = 0, bytesRead = 0, bytesWritten = 0;
  uint64_t readStart = 0, readEnd, writeEnd = 0, serverTime, lastRequest = 0;
  int64_t timeOffset;
  socklen_t addrlen;
  fd_set rfds, wfds;
  FILE *logfile = NULL;
  struct options options;
  struct request *requests;
  struct request_header * requestBuffer;
  struct response_header * responseBuffer;
  struct setup_header setupBuffer;
  struct sockaddr_in addr;
  struct timeval timeout, *timeout_p;

  memset(&options, 0, sizeof(struct options));
  options.argc = argc - 1;
  options.argv = argv + 1;
  options.simul = 1;

  error = optparse(&options);

  if (error || argc != 5) {
    fprintf(stderr, usage, progname);
    return error ? error - 1 : 0;
  }

  if (options.logfilename) {
    logfile = fopen(options.logfilename, "a");
  }

  host = argv[0];
  port = argv[1];
  setupBuffer.requests = atol(argv[2]);
  setupBuffer.request_size = atol(argv[3]);
  setupBuffer.response_size = atol(argv[4]);
  setupBuffer.simul = options.simul;

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
  requests = calloc(setupBuffer.simul + 1, sizeof(struct request));

  /* looks up server and connects */
  if((clientfd = open_socketfd(host, port, AI_V4MAPPED, &connect)) < 0)
  {
    fprintf(stderr, "Error connecting to server %d\n", clientfd);
    return 1;
  }

  LOG(logfile, LOG_LEVEL_V, "connected\n", NULL);

  addrlen = sizeof(addr);
  if (getsockname(clientfd, (struct sockaddr*)&addr, (socklen_t*)&addrlen)) {
    fprintf(stderr, "Error getting socket name\n");
  } else {
    inet_ntop(AF_INET, &addr.sin_addr, addrstr, sizeof(addrstr));
    LOG(logfile, LOG_LEVEL_V, "Connecting from %s:%d\n", addrstr, ntohs(addr.sin_port));
  }

  addrlen = sizeof(addr);
  if (getpeername(clientfd, (struct sockaddr*)&addr, (socklen_t*)&addrlen)) {
    fprintf(stderr, "Error getting socket name\n");
  } else {
    inet_ntop(AF_INET, &addr.sin_addr, addrstr, sizeof(addrstr));
    LOG(logfile, LOG_LEVEL_V, "Connecting to %s:%d\n", addrstr, ntohs(addr.sin_port));
  }

  if (wait) {
    getchar();
  }

  if (options.tcpnodelay) {
    if(setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &options.tcpnodelay, sizeof(int)) == -1)
    {
      fprintf(stderr, "Error setting no delay - continuing\n");
    } else {
      LOG(logfile, LOG_LEVEL_V, "set TCP_NODELAY on socket\n", NULL);
    }
  }

  write(clientfd, &setupBuffer, sizeof(setupBuffer));
  writeEnd = microseconds();
  read(clientfd, &serverTime, sizeof(uint64_t));
  readEnd = microseconds();
  timeOffset = ((readEnd - serverTime) >> 1) + ((writeEnd - serverTime) >> 1);
  LOG(logfile, LOG_LEVEL_V, "calculating an offset of %lld +/- %lld\n", timeOffset, readEnd - writeEnd);


  while (responseCount < setupBuffer.requests) {
    FD_ZERO(&rfds);
    if (responseCount < requestCount) {
      FD_SET(clientfd, &rfds);
    }

    delta = microseconds() - lastRequest;
    FD_ZERO(&wfds);

    SWITCH_TWO(requestCount - responseCount < setupBuffer.simul, delta > options.delay) {
    case 3:
      FD_SET(clientfd, &wfds);
    case 1:
      timeout_p = NULL;
      break;
    case 2:
    case 0:
      timeout.tv_sec = (options.delay - delta) / 1000000L;
      timeout.tv_usec = (options.delay - delta) % 1000000L;
      timeout_p = &timeout;
    }

    select(clientfd+1, &rfds, &wfds, NULL, timeout_p);

    if (FD_ISSET(clientfd, &rfds)) {
      if (!bytesRead) {
        readStart = microseconds();
      }
      n = read(clientfd, ((char*)responseBuffer) + bytesRead, setupBuffer.response_size - bytesRead);
      readEnd = microseconds();
      if (n < 0)
      {
        perror("read: ");
        fprintf(stderr, "Error reading from socket\n");
        break;
      }
      if (n == 0) {
        LOG(logfile, LOG_LEVEL_V, "connection closed\n", NULL);
        break;
      }

      bytesRead += n;
      if (bytesRead == setupBuffer.response_size) {
        bytesRead = 0;
        ++responseCount;

        ir = request_find_slot(requests, responseBuffer->prev_seq, ir, setupBuffer.simul + 1);
        requests[ir].response_write_end = responseBuffer->prev_write_end;
        LOG(logfile, LOG_LEVEL_Q, "seq %lu: %llu %llu %llu %llu %llu %llu %llu %llu *%lld\n",
            requests[ir].seq,
            requests[ir].request_write_start,
            requests[ir].request_write_end,
            requests[ir].request_read_start,
            requests[ir].request_read_end,
            requests[ir].response_write_start,
            requests[ir].response_write_end,
            requests[ir].response_read_start,
            requests[ir].response_read_end,
            timeOffset
        );
        requests[ir].seq = 0;

        ir = request_find_slot(requests, responseBuffer->seq, ir, setupBuffer.simul + 1);
        requests[ir].request_read_start = responseBuffer->read_start;
        requests[ir].request_read_end = responseBuffer->read_end;
        requests[ir].response_write_start = responseBuffer->write_start;
      }
    }

    if (FD_ISSET(clientfd, &wfds)) {
      if (!bytesWritten) {
        iw = request_find_slot(requests, 0, iw, setupBuffer.simul + 1);
        requests[iw].request_write_start = microseconds();
      }
      n = write(clientfd, ((char*)requestBuffer) + bytesWritten, setupBuffer.request_size - bytesWritten);
      requests[iw].request_write_end = microseconds();

      if (n < 0) {
        perror("write: ");
        fprintf(stderr, "Error writing to socket\n");
        break;
      }
      if (n == 0) {
        LOG(logfile, LOG_LEVEL_L, "connection closed\n", NULL);
        break;
      }

      bytesWritten += n;
      if (bytesWritten == setupBuffer.request_size) {
        bytesWritten = 0;
        ++requestCount;
        lastRequest = requests[iw].request_write_end;
      }
    }
  }
  close(clientfd);
  if (logfile) {
    fclose(logfile);
  }
  free(requestBuffer);
  free(responseBuffer);
  free(requests);
  return 0;
}

