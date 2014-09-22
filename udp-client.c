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
#include "udp-shared.h"

struct options {
  int argc;
  char **argv;

  size_t delay, requests, cleanup;
  char *logfilename;
  int *sopriority;
  char *log_level, wait;
};

char *app_type = "client";
static int option_true = 1;
static int option_false = 0;

static const char usage[] =
  "usage: %s [-hqvw] [-c CLEANUP] [-d DELAY] [-s NUM_SIMUL] [-l LOGFILE] [-r REQUESTS] HOST PORT REQUEST_SIZE RESPONSE_SIZE\n"
  "  -c          : How long to wait for all packets to returned (default: no limit)\n"
  "  -d=0        : Delay between consecutive requests\n"
  "  -h          : Print help and exit\n"
  "  -l=/dev/null: Duplicate all statements to a logfile\n"
  "  -p          : Use SO_PRIORITY on socket\n"
  "  -P          : Disable SO_PRIORITY on socket\n"
  "  -q          : Quiet printing\n"
  "  -r          : Number of requests to send (default: no limit)\n"
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
    case 'c': options->cleanup = atoll(options->argv[n++]) + 1; break;
    case 'd': options->delay = atoll(options->argv[n++]); break;
    case 'r': options->requests = atoll(options->argv[n++]); break;
    case 'l': options->logfilename = options->argv[n++]; break;
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

static struct sockaddr_in serveraddr;
static socklen_t serveraddrlen;

static int save(int fd, const struct sockaddr *addr, socklen_t len) {
   memcpy(&serveraddr, addr, len);
   serveraddrlen = len;
   return 0;
}

/* main driver function */
int main(int argc, char **argv)
{
  char *host, *port, *progname = argv[0];
  FILE *logfile = NULL;
  struct options options;
  struct request *request;
  size_t request_size, response_size, buffer_size, requests = 0, responses = 0, delta, lastRequest = 0;
  ssize_t n;
  uint64_t readStart;
  int64_t lowerOffset = 1L << 63, upperOffset = ~(1L << 63);
  int clientfd, error;
  fd_set rfds, wfds;
  struct timeval timeout, *timeout_p;

  memset(&options, 0, sizeof(struct options));
  options.argc = argc - 1;
  options.argv = argv + 1;
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
  request_size = atol(options.argv[2]);
  response_size = atol(options.argv[3]);

  if (request_size < sizeof(struct request)) {
    fprintf(stderr, "REQUEST_SIZE (%lu) must be at least %lu\n", request_size, sizeof(struct request));
    return 1;
  }

  if (response_size < sizeof(struct request)) {
    fprintf(stderr, "RESPONSE_SIZE (%lu) must be at least %lu\n", response_size, sizeof(struct request));
    return 1;
  }

  buffer_size = max(request_size, response_size);

  request = malloc(buffer_size);
  memset(request, 0, buffer_size);

  /* looks up server and connects */
  if ((clientfd = open_socketfd(host, port, AI_V4MAPPED, SOCK_DGRAM, &save)) < 0) {
    fprintf(stderr, "Error connecting to server %d\n", clientfd);
    return 1;
  }

  LOG(logfile, LOG_LEVEL_L, "connected\n");

  if (options.wait) {
    getchar();
  }

  SETSOCKOPT(logfile, LOG_LEVEL_L, clientfd, SOL_SOCKET, SO_PRIORITY, options.sopriority);

  while (!options.requests || responses < options.requests) {
    delta = microseconds() - lastRequest;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    if (responses < requests) {
      FD_SET(clientfd, &rfds);
    }

    if (!options.requests || requests < options.requests) {
      /* more requests to send */
      if (delta > options.delay) {
        FD_SET(clientfd, &wfds);
        timeout_p = NULL;
      } else {
        timeout.tv_sec = (options.delay - delta) / 1000000L;
        timeout.tv_usec = (options.delay - delta) % 1000000L;
        timeout_p = &timeout;
      }
    } else {
      if (options.cleanup && delta > options.cleanup) {
        /* time out waiting for requests */
        LOGF(logfile, LOG_LEVEL_Q, "dropping %lu packets\n", requests - responses);
        break;
      }

      if (options.cleanup) {
        timeout.tv_sec = (options.cleanup - delta) / 1000000L;
        timeout.tv_usec = (options.cleanup - delta) % 1000000L;
        timeout_p = &timeout;
      } else {
        timeout_p = NULL;
      }
    }

    LOG(logfile, LOG_LEVEL_V, "waiting to send messages\n");
    select(clientfd+1, &rfds, &wfds, NULL, timeout_p);

    if (FD_ISSET(clientfd, &wfds)) {
      request->seq = ++requests;
      request->response_len = response_size;
      request->request_write_start = microseconds();
      n = sendto(clientfd, request, request_size, 0, (struct sockaddr*)&serveraddr, serveraddrlen);
      lastRequest = microseconds();
      LOGF(logfile, LOG_LEVEL_V, "sent %d bytes\n", n);
      if (n == -1) {
        fprintf(stderr, "Failed to write to socket\n");
        break;
      }
    }

    if (FD_ISSET(clientfd, &rfds)) {
      readStart = microseconds();
      n = recvfrom(clientfd, request, response_size, 0, NULL, NULL);
      if (n == -1) {
        fprintf(stderr, "Failed to read from socket\n");
        break;
      }
      request->response_read_start = readStart;
      request->response_read_end = microseconds();
      request->response_rcvd = rcvd_microseconds(clientfd);
      LOGF(logfile, LOG_LEVEL_V, "recieved %d bytes\n", n);

      ++responses;
      time_offset(request->request_write_start, request->request_read_end, request->response_write_start, request->response_read_end, &lowerOffset, &upperOffset);
      LOGF(logfile, LOG_LEVEL_Q, "seq %lu: %lu %lu %lu %lu %lu %lu %lu %lu +/- %ld %ld\n",
           request->seq,
           request->request_write_start,
           request->request_rcvd,
           request->request_read_start,
           request->request_read_end,
           request->response_write_start,
           request->response_rcvd,
           request->response_read_start,
           request->response_read_end,
           lowerOffset,
           upperOffset
        );
    }
  }

  close(clientfd);
  if (logfile) {
    fclose(logfile);
  }
  free(request);
  return 0;
}

