#include <pthread.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "traffic-shared.h"
#include "udp-shared.h"
#define LISTEN_MAX 8
#define MAXLINE 256

struct options {
  int argc;
  char **argv;

  char *logfilename;
  int *sopriority;
  char *log_level;
  size_t max_packet_size;
};

static int option_true = 1;
static int option_false = 0;
char* app_type = "server";

static const char usage[] =
  "usage: %s [-aAhnNpPqv] [-l LOGFILE] PORT\n"
  "  -h          : Print help and exit\n"
  "  -l=/dev/null: Duplicate all statements to a logfile\n"
  "  -p          : Use SO_PRIORITY on socket\n"
  "  -P          : Disable SO_PRIORITY on socket\n"
  "  -m=1024     : Maximum packet size\n"
  "  -q          : Quiet printing\n"
  "  -v          : Verbose printing\n"
  ;

static int optparse(struct options *options)
{
  size_t i = 0;
  size_t n = 1;

  while (options->argc >= 2 && options->argv[0][0] == '-') {
    switch(options->argv[0][++i]) {
    case 'l': options->logfilename = &options->argv[0][n++]; break;
    case 'm': options->max_packet_size = atoll(&options->argv[0][n++]);
    case 'p': options->sopriority = &option_true; break;
    case 'P': options->sopriority = &option_false; break;
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


/* driver function */
int main(int argc, char **argv)
{
  char *portstring, *progname = argv[0];
  FILE *logfile = NULL;
  fd_set fds;
  struct options options;
  struct request *request;
  struct sockaddr_in clientaddr;
  int error, listenfd;
  ssize_t n;
  uint64_t selected, readStart;
  socklen_t socklen;

  memset(&options, 0, sizeof(struct options));
  options.argc = argc - 1;
  options.argv = argv + 1;
  options.log_level = &log_level;
  options.max_packet_size = 1024;

  error = optparse(&options);

  if (error || options.argc != 1) {
    fprintf(stderr, usage, progname);
    return error ? error - 1 : 0;
  }

  if (options.logfilename) {
    logfile = fopen(options.logfilename, "a");
  }

  portstring = options.argv[0];

  if (options.max_packet_size < sizeof(struct request)) {
    fprintf(stderr, "Error: Max request size must be larger than struct request\n");

  }

  request = malloc(options.max_packet_size);
  memset(request, 0, options.max_packet_size);

  LOG(logfile, LOG_LEVEL_V, "Opening socket\n");
  listenfd = open_socketfd(NULL, portstring, AI_PASSIVE, SOCK_DGRAM, &bind);
  LOG(logfile, LOG_LEVEL_V, "Opened socket\n");

  if(listenfd < 0) {
    fprintf(stderr, "Error : Cannot listen to socket %s with error %d\n", portstring, listenfd);
    return 1;
  }

  SETSOCKOPT(logfile, LOG_LEVEL_V, listenfd, SOL_SOCKET, SO_REUSEADDR, &option_true);
  SETSOCKOPT(logfile, LOG_LEVEL_L, listenfd, SOL_SOCKET, SO_PRIORITY, options.sopriority);

  while(1) {
loop:
    selected = microseconds();
    LOG(logfile, LOG_LEVEL_V, "Waiting for requests\n");
    select(listenfd + 1, &fds, NULL, NULL, NULL);

    readStart = microseconds();
    n = recvfrom(listenfd, request, options.max_packet_size, 0, (struct sockaddr*)&clientaddr, &socklen);
    LOGF(logfile, LOG_LEVEL_V, "Recieved %d bytes\n", n);

    request->request_sel = selected;
    request->request_read_start = readStart;
    request->request_read_end = microseconds();
    request->request_rcvd = rcvd_microseconds(listenfd);

    if (n < sizeof(struct request)) {
      LOGF(logfile, LOG_LEVEL_L, "Packet too small (%d < %lu) dropping.\n", n, sizeof(struct request));
      goto loop;
    }

    if (request->response_len > options.max_packet_size) {
      LOGF(logfile, LOG_LEVEL_L, "Response packet size requested is too large (%lu > %lu), truncating\n", request->response_len, options.max_packet_size);
      request->response_len = options.max_packet_size;
    }

    request->response_write_start = microseconds();
    n = sendto(listenfd, request, request->response_len, 0, (struct sockaddr*)&clientaddr, socklen);
    LOGF(logfile, LOG_LEVEL_V, "Sent %d bytes\n", n);
  }
}
