#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <netdb.h>
#include "traffic-shared.h"

char log_level = LOG_LEVEL_L;

uint64_t microseconds(void)
{
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return tv.tv_sec * (uint64_t) 1000000 + tv.tv_usec;
}

int open_socketfd(char *hostname, char* port, int flags, int (*func)(int, const struct sockaddr*, socklen_t))
{
  int socketfd;
  struct addrinfo hints, *hostaddresses = NULL;
  memset(&hints, 0, sizeof(hints));
  /* ai_flags indicating what type of connection there is
     flags is expected to be either PASSIVE or V4_MAPPED */
  hints.ai_flags = AI_ADDRCONFIG | flags;
  /* indicates an ip4 address */
  hints.ai_family = AF_INET;
  /* looking for a stream connection */
  hints.ai_socktype = SOCK_STREAM;
  /* asks the DNS server for the address */
  if(getaddrinfo(hostname, port, &hints, &hostaddresses) != 0)
    return -3;
  /* creates a socket to that address */
  if((socketfd = socket(hostaddresses->ai_family, hostaddresses->ai_socktype, hostaddresses->ai_protocol)) == -1)
    return -2;
  /* either binds or connects to the address */
  if(func(socketfd, hostaddresses->ai_addr, hostaddresses->ai_addrlen) == -1)
    return -1;
  freeaddrinfo(hostaddresses);
  return socketfd;
}

/* wrapper function to printing to the screen
 * handles special characters more cleanly
 */
void fputs2(FILE* out, char* buf, size_t n)
{
  int count;
  for(count = 0; n-- > 0; count++)
  {
    if(buf[count] < 32 /*&& buf[count] != '\n'*/)
      fprintf(out, "\\x%02x", (int)buf[count]);
    else
      fputc(buf[count], out);
  }
}

/* wrapper function to read from input,
 * handles null characters, and breaks at
 * newlines.
 */
int fgets2(FILE* in, char* buf, size_t n)
{
  int count = 0, c;
  while(n-- > 0)
  {
    if((c = fgetc(in)) == EOF)
      return -1;
    buf[count++] = (char)c;
    if(c == '\n')
      return count;
  }
  return count;
}

size_t request_find_slot(struct request* requests, size_t seq, size_t last, size_t size) {
  for (; requests[last].seq != seq; last = (last + 1) % size) ;
  return last;
}

int getintsockopt(int sockfd, int level, int optname) {
  socklen_t len = sizeof(int);
  int option, error;

  error = getsockopt(sockfd, level, optname, &option, &len);

  if (error) {
    return error;
  }
  return option;
}

void setintsockopt(FILE *logfile, int loglevel, int fd, int level, int optname, char *optstring, int *optval) {
  if (optval) {
    if (setsockopt(fd, level, optname, optval, sizeof(int)) == -1) {
      fprintf(stderr, "Error setting socket option %s on %d - continuing\n", optstring, fd);
    } else {
      LOGF(logfile, loglevel, "set %s to %d on socket\n", optstring, *optval);
    }
  }
}

void logintsockopt(FILE *logfile, int loglevel, int fd, int level, int optname, char *optstring) {
  socklen_t len = sizeof(int);
  int optval, error;

  error = getsockopt(fd, level, optname, &optval, &len);
  if (error == -1) {
    fprintf(stderr, "Error getting socket option %s on %d - continuing\n", optstring, fd);
  } else {
    LOGF(logfile, loglevel, "%s is set to %d on socket\n", optstring, optval);
  }
}

