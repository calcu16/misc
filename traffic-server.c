#include <pthread.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "traffic-shared.h"
#define LISTEN_MAX 8
#define MAXLINE 256
#define TYPE "server"

struct options {
  int argc;
  char **argv;

  char *logfilename;
  char tcpnodelay, verbose;
};

static const char usage[] =
  "usage: %s [-hnv] [-l LOGFILE] PORT\n"
  "  -h          : Print help and exit\n"
  "  -l=/dev/null: Duplicate all statements to a logfile\n"
  "  -n          : Use tcp no delay on incoming connections\n"
  "  -v          : Verbose printing\n"
  ;

/* cleans up the zombie processes */
void* clean(void* v)
{
  sem_t* count = v;
  int pid, status;
  while(1)
  {
    /* waits for a new process to be spawned */
    sem_wait(count);
    /* waits for that process to finish */
    pid = wait(&status);
    /* a child process has finished, rinse and repeat */
    printf("server: connection of %d closed with status %d\n", pid, status);
  }
}

int respond(int connfd, size_t port, FILE * logfile, char verbose)
{
  struct setup_header setupBuffer;
  struct request_list requests = NEW_LIST;
  size_t  bytesRead = 0, requestCount = 0, bytesWritten = 0, responseCount = 0;
  ssize_t n;
  uint64_t readStart = 0, readEnd, writeStart = 0, writeEnd;
  struct request_header * requestBuffer;
  struct response_header * responseBuffer;
  fd_set rfds, wfds;

  n = read(connfd, &setupBuffer, sizeof(struct setup_header));
  readEnd = microseconds();
  if (n != sizeof(struct setup_header)) {
    fprintf(stderr, "Failed to read setup from connection on port %lu\n", port);
    return -1;
  }
  write(connfd, &readEnd, sizeof(uint64_t));

  LOG(logfile, "client %lu sending %lu requests of size %lu expecting responses of size %lu\n", port, setupBuffer.requests, setupBuffer.request_size, setupBuffer.response_size);

  requestBuffer = malloc(setupBuffer.request_size);
  responseBuffer = malloc(setupBuffer.response_size);
  responseBuffer->prev_write_end = microseconds();

  while (responseCount < setupBuffer.requests) {
    FD_ZERO(&rfds);
    if (requestCount < setupBuffer.requests) {
      FD_SET(connfd, &rfds);
    }

    FD_ZERO(&wfds);
    if (responseCount < requestCount) {
      FD_SET(connfd, &wfds);
    }

    printf("%lu server: selecting requests, %lu requests recieved %lu responses written\n", microseconds(), requestCount, responseCount);
    select(connfd+1, &rfds, &wfds, NULL, NULL);

    if (FD_ISSET(connfd, &rfds)) {
      printf("%lu server: reading %ld bytes from port %lu\n", microseconds(), setupBuffer.request_size - bytesRead, port);

      if (bytesRead == 0) {
        printf("%lu server: reading next request\n", microseconds());
        readStart = microseconds();
      }

      n = read(connfd, ((char*)requestBuffer) + bytesRead, setupBuffer.request_size - bytesRead);

      if (n < 0) {
        perror("read: ");
        fprintf(stderr, "Failed to read request from connection on port %lu\n", port);
        return -1;
      }
      if (n == 0) {
        fprintf(stderr, "Connection on port %lu closed during read\n", port);
        return -1;
      }
      readEnd = microseconds();

      printf("%lu server: read %lu bytes from port %lu\n", microseconds(), n, port);
      bytesRead +=n;

      if (bytesRead == setupBuffer.request_size) {
        printf("%lu server: finished reading request\n", microseconds());
        requests = append_request(requests, requestBuffer->seq, readStart, readEnd);
        ++requestCount;
        bytesRead = 0;
      }
    }

    if (FD_ISSET(connfd, &wfds)) {
      printf("%lu server: writing %ld bytes to port %lu\n", microseconds(), setupBuffer.response_size - bytesRead, port);

      if (bytesWritten == 0) {
        printf("%lu server: writing next response\n", microseconds());
        writeStart = microseconds();
      }

      responseBuffer->seq = requests.head->seq;
      responseBuffer->read_start = requests.head->read_start;
      responseBuffer->read_end = requests.head->read_end;
      responseBuffer->write_start = writeStart;

      n = write(connfd, ((char*)responseBuffer) + bytesWritten, setupBuffer.response_size - bytesWritten);

      if (n < 0) {
        perror("write: ");
        fprintf(stderr, "Failed to write request to connection on port %lu\n", port);
        return -1;
      }
      if (n == 0) {
        fprintf(stderr, "Connection on port %lu closed during write\n", port);
        return -1;
      }
      writeEnd = microseconds();

      printf("%lu server: wrote %lu bytes to port %lu\n", microseconds(), n, port);
      bytesWritten += n;

      if (bytesWritten == setupBuffer.response_size) {
        printf("%lu server: finished writing response\n", microseconds());
        responseBuffer->prev_seq = responseBuffer->seq;
        responseBuffer->prev_write_end = writeEnd;
        requests = remove_request(requests);
        ++responseCount;
        bytesWritten = 0;
      }
    }
  }
  return 0;
}


static int optparse(struct options *options);
  size_t i = 0;
  size_t n = 1;

  --(options->argc);
  ++(options->argv);

  while (options->argc >= 2 && options->argv[0][0] == '-') {
    switch(options->argv[0][++i]) {
    case 'l': options->logfilename = argv[0][n++]; break;
    case 'n': options->tcpnodelay = 1; break;
    case 'v': options->verbose = 1; break;
    case 'h': return 1;
    case '-': *argc -= n; *argv += n return 0;
    case 0:
      *argc -= n;
      *argv += n;
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
  char hostaddr[MAXLINE], hostname[MAXLINE], *progname = argv[0], *portstring;
  int listenfd, connfd, error, pid, threaded = 0, optval = 1;
  size_t port, total = 0;
  FILE *logfile = NULL;
  sem_t count;
  struct options options;
  pthread_t cleaning;
  socklen_t clientlen;
  union
  {
    struct sockaddr_in client4;
    struct sockaddr_in6 client6;
  } clientaddr;

  if (sem_init(&count, 0, 0) == -1 || pthread_create(&cleaning, NULL, clean, &count) != 0) {
    fprintf(stderr, "Warning : Unable to initialize thread mechanisms, child processes may not be cleaned up\n");
  } else {
    threaded = 1;
  }

  memset(&options, 0, sizeof(struct options));
  options->argc = argc - 1;
  options->argv = argv + 1;

  error = optparse(&options);

  if (error || argc != 1) {
    fprintf(stderr, usage, progname);
    return error ? error - 1 : 0;
  }

  if (options->logfilename) {
    logfile = fopen(options->logfilename, "a");
  }

  portstring = options->argv[0];

  listenfd = open_socketfd(NULL, portstring, AI_PASSIVE, &bind);

  if(listenfd < 0) {
    fprintf(stderr, "Error : Cannot listen to socket %s with error %d\n", portstring, listenfd);
    return 1;
  }

  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(optval)) == -1) {
    fprintf(stderr, "Error : Unable to set socket options\n");
    return 1;
  }

  if (listen(listenfd, LISTEN_MAX) == -1) {
    fprintf(stderr, "Error : Cannot listen on port\n");
    return 1;
  }
  LOG("server: listening\n");

  while(1)
  {
    clientlen = sizeof(clientaddr);
    connfd = accept(listenfd, (void *)(&clientaddr), &clientlen);
    if (connfd == -1) {
      continue;
    }
    error = getnameinfo((struct sockaddr*)&clientaddr, clientlen, hostname, sizeof(hostname), NULL, 0, 0);
    if (error != 0) {
      close(connfd);
      continue;
    }
    error = getnameinfo((struct sockaddr*)&clientaddr, clientlen, hostaddr, sizeof(hostaddr), NULL, 0, NI_NUMERICHOST);

    port = ntohs(((struct sockaddr*)&clientaddr)->sa_family == AF_INET
            ? ((struct sockaddr_in*)&clientaddr)->sin_port
            : ((struct sockaddr_in6*)&clientaddr)->sin6_port);
    if (error == 0) {
      printf("server: connected to %s (%s) : %lu\n", hostname, hostaddr, port);
    } else {
      printf("server: server connected to %s : %lu\n", hostname, port);
    }

    if ((pid = fork()) == 0) {
      close(listenfd);
      setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, (char*)&tcp_nodelay, sizeof(int));
      respond(connfd, port);
      LOG(logfile, "server: closing connection to %s (%s) : %lu\n", hostname, hostaddr, port);
      if (logfile) {
        fclose(logfile);
      }
      close(connfd);
      exit(0);
    }
    ++total;
    close(connfd);
    if (pid > 0) {
      if (threaded && sem_post(&count) == -1) {
        fprintf(stderr, "Warning : Semaphore overflow, child processes may not be cleaned up\n");
      }
      LOG(logfile, "server: forked to pid %d, connection accepted\n", pid);
    }
    if(pid == -1) {
      fprintf(stderr, "Error : Failed to fork, connection refused\n");
    }
  }
}
