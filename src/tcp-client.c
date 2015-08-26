#define MAIN
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
#include "tcp-shared.h"

#define SWITCH_TWO(a,b) switch(!!(a) << 1 | !!(b))

struct options {
  int argc;
  char **argv;

  size_t delay, requests, simul;
  char *logfilename;
  int *tcpquickack, *tcpnodelay, *sopriority;
  char *log_level, wait;
};

char *app_type = "client";
static int option_true = 1;
static int option_false = 0;

static const char usage[] =
  "usage: %s [-hnqvw] [-d DELAY] [-s NUM_SIMUL] [-l LOGFILE] [-r REQUESTS] HOST PORT REQUEST_SIZE RESPONSE_SIZE\n"
  "  -a          : TCP Quick Ack\n"
  "  -A          : Disable tcp quick ack on outgoing connections\n"
  "  -d=0        : Delay between consecutive requests\n"
  "  -h          : Print help and exit\n"
  "  -l=/dev/null: Duplicate all statements to a logfile\n"
  "  -n          : Use tcp no delay on outgoing connections\n"
  "  -N          : Do not use tcp no delay on outgoing connections\n"
  "  -p          : Use SO_PRIORITY on socket\n"
  "  -P          : Disable SO_PRIORITY on socket\n"
  "  -q          : Quiet printing\n"
  "  -r          : Number of requests to send (default: no limit)\n"
  "  -s=1        : Allow for NUM_SIMUL requests at the same time\n"
  "  -v          : Verbose printing\n"
  "  -w          : Wait for input from stdin after connecting but before sending the normal requests\n"
  ;

static int64_t min(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t max(int64_t a, int64_t b) { return a > b ? a : b; }

static int time_offset(uint64_t requestWrite, uint64_t requestRead, uint64_t responseWrite, uint64_t responseRead, int64_t *lowerOffset, int64_t *upperOffset) {
  uint64_t transit = (responseRead - requestWrite) - (responseWrite - requestRead);
  int64_t newLowerOffset = min(requestWrite - requestRead, responseRead - responseWrite) - transit;
  int64_t newUpperOffset = max(requestWrite - requestRead, responseRead - responseWrite) + transit;
  int ret = ((newLowerOffset > *lowerOffset) << 1) | (newUpperOffset < *upperOffset);

  if (newUpperOffset < *lowerOffset || newLowerOffset > *upperOffset) {
    /* drift */
    *lowerOffset = newLowerOffset;
    *upperOffset = newUpperOffset;
    return 4;
  }

  *lowerOffset = max(*lowerOffset, newLowerOffset);
  *upperOffset = min(*upperOffset, newUpperOffset);
  return ret;
}

static int optparse(struct options *options)
{
  size_t i = 0;
  size_t n = 1;

  while (options->argc >= 2 && options->argv[0][0] == '-') {
    switch(options->argv[0][++i]) {
    case 'd': options->delay = atoll(options->argv[n++]); break;
    case 'r': options->requests = atoll(options->argv[n++]); break;
    case 's': options->simul = atoll(options->argv[n++]); break;
    case 'l': options->logfilename = options->argv[n++]; break;
    case 'a': options->tcpquickack = &option_true; break;
    case 'A': options->tcpquickack = &option_false; break;
    case 'n': options->tcpnodelay = &option_true; break;
    case 'N': options->tcpnodelay = &option_false; break;
    case 'p': options->sopriority = &option_true; break;
    case 'P': options->sopriority = &option_false; break;
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
  uint64_t readStart = 0, readEnd, writeStart = 0, serverTime, lastRequest = 0;
  int64_t lowerOffset = 1L << 63, upperOffset = ~(1L << 63);
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
  options.log_level = &log_level;

  error = optparse(&options);

  if (error || options.argc != 4) {
    fprintf(stderr, usage, progname);
    return error ? error - 1 : 0;
  }

  if (options.logfilename) {
    logfile = fopen(options.logfilename, "a");
  }

  host = options.argv[0];
  port = options.argv[1];
  setupBuffer.request_size = atol(options.argv[2]);
  setupBuffer.response_size = atol(options.argv[3]);
  setupBuffer.simul = options.simul;
  setupBuffer.requests = options.requests;

  if (setupBuffer.request_size < sizeof(struct request_header)) {
    fprintf(stderr, "REQUEST_SIZE (%lu) must be at least %lu\n", setupBuffer.request_size, sizeof(struct request_header));
    return 1;
  }

  if (setupBuffer.response_size < sizeof(struct response_header)) {
    fprintf(stderr, "RESPONSE_SIZE (%lu) must be at least %lu\n", setupBuffer.response_size, sizeof(struct response_header));
    return 1;
  }

  requestBuffer = malloc(setupBuffer.request_size);
  responseBuffer = malloc(setupBuffer.response_size);
  requests = calloc(setupBuffer.simul + 1, sizeof(struct request));

  /* looks up server and connects */
  if((clientfd = open_socketfd(host, port, AI_V4MAPPED, SOCK_STREAM, &connect)) < 0)
  {
    fprintf(stderr, "Error connecting to server %d\n", clientfd);
    return 1;
  }

  LOG(logfile, LOG_LEVEL_L, "connected\n");

  addrlen = sizeof(addr);
  if (getsockname(clientfd, (struct sockaddr*)&addr, (socklen_t*)&addrlen)) {
    fprintf(stderr, "Error getting socket name\n");
  } else {
    inet_ntop(AF_INET, &addr.sin_addr, addrstr, sizeof(addrstr));
    LOGF(logfile, LOG_LEVEL_L, "Connected from %s:%d\n", addrstr, ntohs(addr.sin_port));
  }

  addrlen = sizeof(addr);
  if (getpeername(clientfd, (struct sockaddr*)&addr, (socklen_t*)&addrlen)) {
    fprintf(stderr, "Error getting socket name\n");
  } else {
    inet_ntop(AF_INET, &addr.sin_addr, addrstr, sizeof(addrstr));
    LOGF(logfile, LOG_LEVEL_L, "Connected to %s:%d\n", addrstr, ntohs(addr.sin_port));
  }

  if (options.wait) {
    getchar();
  }

  SETSOCKOPT(logfile, LOG_LEVEL_L, clientfd, SOL_SOCKET, SO_PRIORITY, options.sopriority);
  SETSOCKOPT(logfile, LOG_LEVEL_L, clientfd, IPPROTO_TCP, TCP_NODELAY, options.tcpnodelay);
  SETSOCKOPT(logfile, LOG_LEVEL_L, clientfd, IPPROTO_TCP, TCP_QUICKACK, options.tcpquickack);

  writeStart = microseconds();
  write(clientfd, &setupBuffer, sizeof(setupBuffer));


  read(clientfd, &serverTime, sizeof(uint64_t));
  readEnd = microseconds();
  LOGF(logfile, LOG_LEVEL_V, "initial time offset %lu-%lu-%lu deltas of %ld %ld and transit time %lu\n", writeStart, serverTime, readEnd, serverTime - writeStart, readEnd - serverTime, readEnd - writeStart);
  time_offset(writeStart, serverTime, serverTime, readEnd, &lowerOffset, &upperOffset);
  LOGF(logfile, LOG_LEVEL_V, "calculating an initial offset of +/- %ld %ld\n", lowerOffset, upperOffset);
  LOGSOCKOPT(logfile, LOG_LEVEL_L, clientfd, SOL_SOCKET, SO_PRIORITY);
  LOGSOCKOPT(logfile, LOG_LEVEL_L, clientfd, IPPROTO_TCP, TCP_NODELAY);
  LOGSOCKOPT(logfile, LOG_LEVEL_L, clientfd, IPPROTO_TCP, TCP_QUICKACK);


  while (!setupBuffer.requests || responseCount < setupBuffer.requests) {
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
      break;
    default:
      timeout_p = NULL;
    }

    LOGSOCKOPT(logfile, LOG_LEVEL_V, clientfd, IPPROTO_TCP, TCP_NODELAY);
    LOGSOCKOPT(logfile, LOG_LEVEL_V, clientfd, IPPROTO_TCP, TCP_QUICKACK);
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
        LOG(logfile, LOG_LEVEL_V, "connection closed\n");
        break;
      }
      LOGF(logfile, LOG_LEVEL_V, "read %ld bytes from socket\n", n);

      bytesRead += n;
      if (bytesRead == setupBuffer.response_size) {
        bytesRead = 0;
        ++responseCount;
        LOGF(logfile, LOG_LEVEL_V, "recieved response prev-seq %lu: %lu at %ld, seq %lu: %lu %lu %lu at %lu\n",
            responseBuffer->prev_seq,
            responseBuffer->prev_write_end,
            (ssize_t)(responseBuffer->prev_index - 1),
            responseBuffer->seq,
            responseBuffer->read_start,
            responseBuffer->read_end,
            responseBuffer->write_start,
            responseBuffer->index - 1
        );

        if (responseBuffer->prev_index) {
          ir = responseBuffer->prev_index - 1;
          if (ir > setupBuffer.simul) {
            fprintf(stderr, "Error: previous index %lu is out of range (limit %lu)\n", ir, setupBuffer.simul);
            break;
          }
          if (requests[ir].seq != responseBuffer->prev_seq) {
            fprintf(stderr, "Error: previous index %lu contains seq %lu (recieved %lu)\n", ir, requests[ir].seq, responseBuffer->prev_seq);
            break;
          }
          LOGF(logfile, LOG_LEVEL_V, "finding slot for %lu\n", responseBuffer->prev_seq);
          requests[ir].response_write_end = responseBuffer->prev_write_end;
          time_offset(requests[ir].request_write_start, requests[ir].request_read_end, requests[ir].response_write_start, requests[ir].response_read_end, &lowerOffset, &upperOffset);
          LOGF(logfile, LOG_LEVEL_Q, "seq %lu: %lu %lu %lu %lu %lu %lu %lu %lu +/- %ld %ld\n",
              requests[ir].seq,
              requests[ir].request_write_start,
              requests[ir].request_write_end,
              requests[ir].request_read_start,
              requests[ir].request_read_end,
              requests[ir].response_write_start,
              requests[ir].response_write_end,
              requests[ir].response_read_start,
              requests[ir].response_read_end,
              lowerOffset,
              upperOffset
          );
          requests[ir].seq = 0;
        }

        ir = responseBuffer->index - 1;
        if (ir > setupBuffer.simul) {
            fprintf(stderr, "Error: index %lu is out of range (limit %lu)\n", ir, setupBuffer.simul);
            break;
        }
        if (requests[ir].seq != responseBuffer->seq) {
          fprintf(stderr, "Error: index %lu contains seq %lu (recieved %lu)\n", ir, requests[ir].seq, responseBuffer->seq);
          break;
        }
        requests[ir].request_read_start = responseBuffer->read_start;
        requests[ir].request_read_end = responseBuffer->read_end;
        requests[ir].response_write_start = responseBuffer->write_start;
        requests[ir].response_read_start = readStart;
        requests[ir].response_read_end = readEnd;
      }
    }

    if (FD_ISSET(clientfd, &wfds)) {
      LOGSOCKOPT(logfile, LOG_LEVEL_V, clientfd, IPPROTO_TCP, TCP_QUICKACK);
      if (!bytesWritten) {
        iw = request_find_slot(requests, 0, iw, setupBuffer.simul + 1);
        requestBuffer->seq = requestCount + 1;
        requestBuffer->index = iw + 1;
        requests[iw].request_write_start = microseconds();
      }
      n = write(clientfd, ((char*)requestBuffer) + bytesWritten, setupBuffer.request_size - bytesWritten);
      requests[iw].request_write_end = microseconds();

      SETSOCKOPT(logfile, LOG_LEVEL_V, clientfd, IPPROTO_TCP, TCP_QUICKACK, options.tcpquickack);

      if (n < 0) {
        perror("write: ");
        fprintf(stderr, "Error writing to socket\n");
        break;
      }
      if (n == 0) {
        LOG(logfile, LOG_LEVEL_L, "connection closed\n");
        break;
      }

      LOGF(logfile, LOG_LEVEL_V, "wrote %ld bytes to socket\n", n);

      bytesWritten += n;
      if (bytesWritten == setupBuffer.request_size) {
        bytesWritten = 0;
        ++requestCount;
        requests[iw].seq = requestCount;
        lastRequest = requests[iw].request_write_end;
        LOGF(logfile, LOG_LEVEL_V, "saved %lu, %lu, %lu to index %lu\n", requests[iw].seq, requests[iw].request_write_start, requests[iw].request_write_end, iw);
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

