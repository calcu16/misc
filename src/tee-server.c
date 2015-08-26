#define MAIN
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
#define MAXLINE 4096

struct options {
  int argc;
  char **argv;
};

char* app_type = "server";

static const char usage[] =
  "usage: %s PORT\n"
  "  -h          : Print help and exit\n";

static int optparse(struct options *options)
{
  size_t i = 0;
  size_t n = 1;

  while (options->argc >= 2 && options->argv[0][0] == '-') {
    switch(options->argv[0][++i]) {
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
  char *portstring, *progname = argv[0], buffer[MAXLINE];
  struct options options;
  struct sockaddr_in clientaddr;
  int error, listenfd;
  ssize_t n;
  socklen_t socklen;

  memset(&options, 0, sizeof(struct options));
  options.argc = argc - 1;
  options.argv = argv + 1;

  error = optparse(&options);

  if (error || options.argc != 1) {
    fprintf(stderr, usage, progname);
    return error ? error - 1 : 0;
  }

  portstring = options.argv[0];
  listenfd = open_socketfd(NULL, portstring, AI_PASSIVE, SOCK_DGRAM, &bind);

  if(listenfd < 0) {
    fprintf(stderr, "Error : Cannot listen to socket %s with error %d\n", portstring, listenfd);
    return 1;
  }

  while(1) {
    n = recvfrom(listenfd, buffer, MAXLINE, 0, (struct sockaddr*)&clientaddr, &socklen);
    write(1, buffer, n);
  }
}
