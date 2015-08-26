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
#include "udp-shared.h"
#define MAXLINE 4096

struct options {
  int argc;
  char **argv;
};

char *app_type = "client";

static const char usage[] =
  "usage: %s HOST PORT\n"
  "  -h          : Print help and exit\n"
  ;

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
  char *host, *port, *progname = argv[0], buffer[MAXLINE];
  struct options options;
  ssize_t n = 1, c = 0, i;
  int clientfd, error;

  memset(&options, 0, sizeof(struct options));
  options.argc = argc - 1;
  options.argv = argv + 1;

  error = optparse(&options);

  if (error || options.argc != 2) {
    fprintf(stderr, usage, progname);
    return error ? error - 1 : 0;
  }

  host = options.argv[0];
  port = options.argv[1];

  /* looks up server and connects */
  if ((clientfd = open_socketfd(host, port, AI_V4MAPPED, SOCK_DGRAM, &save)) < 0) {
    fprintf(stderr, "Error connecting to server %d\n", clientfd);
    return 1;
  }

  while (1) {
    n = read(0, buffer + c, MAXLINE - c);
    if (n == 0) {
      exit(0);
    }
    c += n;
    for (i = 0; i < c; ) {
      if (buffer[i++] == '\n') {
        write(1, buffer, i);
        sendto(clientfd, buffer, i, 0, (struct sockaddr*)&serveraddr, serveraddrlen);
        memmove(buffer, buffer + i, c - i);
        c -= i;
        i = 0;
      }
    }
    if (c == MAXLINE) {
      fprintf(stderr, "Line too long, dropping %d characters\n", MAXLINE);
    }
  }

  close(clientfd);
  return 0;
}

