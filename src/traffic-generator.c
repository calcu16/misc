#define MAIN
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#define MAXLINE 2048
static const char USAGE[] =
  "Usage: %s [-d DELAY] [-n NAME] [-p PRINT_FREQ] [-r NUM_REQUESTS] [-t NUM_THREADS] [-w WAIT_TIME] FILE HOST PORT\n"
  "Generate traffic based on the contents of FILE to a given HOST:PORT\n"
  "\n"
  "  -d     The delay in microseconds between requests (default: 0)\n"
  "  -n     A test name to insert into the message to keep track of\n"
  "  -p     The frequence at which to print information (default: never)\n"
  "  -r     The number of requests to send (default: no limit)\n"
  "  -t     The number of threads to use (default: 1)\n"
  "  -w     The time in microseconds to wait after sending all requests before closing the connection (default: 0)\n";

char *host, *port, *name;
char request_format[MAXLINE];
size_t requests_arg = -1;
size_t delay_arg = 0;
size_t threads = 1;
size_t wait_time = 0;

struct stats {
  size_t requests;
  size_t response_bytes;
};
volatile struct stats * stats;

uint64_t microseconds(void) {
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return tv.tv_sec * (uint64_t) 1000000 + tv.tv_usec;
}

int open_socketfd(char *hostname, char* portnum, int flags, int (*func)(int, const struct sockaddr*, socklen_t)) {
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
  if(getaddrinfo(hostname, portnum, &hints, &hostaddresses) != 0) {
    return -3;
  }
  /* creates a socket to that address */
  if((socketfd = socket(hostaddresses->ai_family, hostaddresses->ai_socktype, hostaddresses->ai_protocol)) == -1) {
    freeaddrinfo(hostaddresses);
    return -2;
  }
  /* either binds or connects to the address */
  if(connect(socketfd, hostaddresses->ai_addr, hostaddresses->ai_addrlen) == -1) {
    freeaddrinfo(hostaddresses);
    close(socketfd);
    return -1;
  }
  freeaddrinfo(hostaddresses);
  return socketfd;
}

void send_requests(int threadid) {
  size_t last_request, delta;
  size_t requests = requests_arg / threads + ((requests_arg % threads) > threadid), delay = delay_arg * threads;
  fd_set rfds, wfds;
  struct timeval timeout;
  char buf[MAXLINE];
  int n, clientfd = -1;
  char tcp_nodelay = 1;

  /* looks up server and connects */
  if((clientfd = open_socketfd(host, port, AI_V4MAPPED, &connect)) < 0) {
    /* NOTE: printf/fprintf don't actually work multithreaded, so lines can occaisionally get jumbled */
    fprintf(stderr, "Error connecting to server %s:%s returned %d\n", host, port, clientfd);
    return;
  }
  if(setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, (char*)&tcp_nodelay, sizeof(int)) == -1) {
    fprintf(stderr, "Error setting no delay - continuing\n");
  }

  last_request = microseconds() + delay_arg * threadid;
  while (stats[threadid].requests < requests) {
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(clientfd, &rfds);

    delta = microseconds() - last_request;
    if (delta > delay) {
      FD_SET(clientfd, &wfds);
      timeout.tv_sec = 0;
      timeout.tv_usec = 0;
    } else {
      timeout.tv_sec = (delay - delta) / 1000000L;
      timeout.tv_usec = (delay - delta) % 1000000L;
    }

    select(clientfd+1, &rfds, &wfds, NULL, (delta > delay) ? NULL : &timeout);

    if (FD_ISSET(clientfd, &rfds)) {
      n = read(clientfd, buf, sizeof(buf));
      if (n < 0) {
        fprintf(stderr, "Error reading from socket\n");
        break;
      }
      if (n == 0) {
        printf("%lu client: connection closed\n", microseconds());
        break;
      }
      stats[threadid].response_bytes += n;
    }
    if (FD_ISSET(clientfd, &wfds)) {
      n = snprintf(buf, MAXLINE, request_format, stats[threadid].requests * threads + threadid, name);
      if (n >= MAXLINE) {
        fprintf(stderr, "Request larger than max line\n");
        return;
      }
      n = write(clientfd, buf, n);
      ++stats[threadid].requests;
      if (n == 0) {
        break;
      }
      last_request = microseconds();
    }
  }

  /* wait for any stragglers to trickle in */
  while (1) {
    FD_ZERO(&rfds);
    FD_SET(clientfd, &rfds);
    delta = microseconds() - last_request;
    if (delta > wait_time) {
      break;
    }
    timeout.tv_sec = (delay - delta) / 1000000L;
    timeout.tv_usec = (delay - delta) % 1000000L;

    select(clientfd+1, &rfds, NULL, NULL, &timeout);
    n = read(clientfd, buf, sizeof(buf));
    if (n < 0) {
      fprintf(stderr, "Error reading from socket\n");
      break;
    }
    if (n == 0) {
      printf("%lu client: connection closed\n", microseconds());
      break;
    }
  }
  close(clientfd);
}

/* main driver function */
int main(int argc, char **argv)
{
  char * file;
  char * argv0 = argv[0];
  size_t n, i = 0, j = 1;
  int fd;
  size_t print_arg = -1;
  sigset_t sigchldset;
  size_t last_print;
  struct stats current_stats;
  struct stats last_stats = { 0, 0 };
  size_t now;
  struct timespec timeout;

  name = argv[0];

  while (argc >= 3 && argv[1][0] == '-') {
    switch(argv[1][++i]) {
    case 0:
      argc -= j;
      argv += j;
      i = 0;
      j = 1;
      break;
    case 'd':
      delay_arg = atol(argv[++j]);
      break;
    case 'n':
      name = argv[++j];
      break;
    case 'p':
      print_arg = atol(argv[++j]);
      break;
    case 'r':
      requests_arg = atol(argv[++j]);
      break;
    case 't':
      threads = atol(argv[++j]);
      break;
    case 'w':
      wait_time = atol(argv[++j]);
      break;
    default:
      fprintf(stderr, "Unrecognized option '%c'\n", argv[1][1]);
      return 1;
    }
  }

  if (argc < 4) {
    fprintf(stderr, USAGE, argv0);
    return 1;
  }

  file = argv[1];
  host = argv[2];
  port = argv[3];

  fd = open(file, O_RDONLY);
  if (fd == -1) {
    fprintf(stderr, "Unable to read '%s'\n", file);
    return 1;
  }
  n = read(fd, request_format, MAXLINE);
  if (n == MAXLINE) {
    fprintf(stderr, "Warning: file exceeds max request length of 2048, truncating\n");
  }
  close(fd);

  /* should probably test for failure conditions */
  stats = mmap(NULL, sizeof(size_t) * threads, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
  if (stats  == MAP_FAILED) {
    fprintf(stderr, "Failed to map shared memory\n");
    return 1;
  }

  sigemptyset(&sigchldset);
  sigaddset(&sigchldset, SIGCHLD);
  sigprocmask(SIG_BLOCK, &sigchldset, NULL);

  for (i = 0; i < threads; ++i) {
    switch (fork()) {
    case -1:
      fprintf(stderr, "Fork failed\n");
      return 1;
    case 0:
      prctl(PR_SET_PDEATHSIG, SIGHUP);
      send_requests(i);
      return 0;
    default:
      ;
    }
  }

  last_print = microseconds();
  for (i = 0; i < threads; ) {
    now = microseconds();
    if ((now - last_print) >= print_arg) {
      memset(&current_stats, 0, sizeof(current_stats));
      for (j = 0; j < threads; ++j) {
        current_stats.requests += stats[j].requests;
        current_stats.response_bytes += stats[j].response_bytes;
      }
      printf("Average requests per second: %.2f, response MB/S %.2f\n",
          (current_stats.requests - last_stats.requests) * 1000000.0 / (now - last_print),
          (current_stats.response_bytes - last_stats.response_bytes) * 0.9536743164 / (now - last_print));
      last_stats = current_stats;
      last_print = now;
    }
    timeout.tv_sec = (print_arg + last_print - now)  / 1000000000UL;
    timeout.tv_nsec = ((print_arg + last_print - now) % 1000000UL) * 1000;

    n = sigtimedwait(&sigchldset, NULL, &timeout);
    for (; 0 < waitpid(-1, NULL, WNOHANG); ++i) ;
  }
  return 0;
}
