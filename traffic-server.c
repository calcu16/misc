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
#define LISTEN_MAX 8
#define MAXLINE 256

struct options {
  int argc;
  char **argv;

  char *logfilename;
  int *tcpquickack, *tcpnodelay;
  char *log_level;
};

static int option_true = 1;
static int option_false = 0;
char* app_type = "server";

static const char usage[] =
  "usage: %s [-ahnwv] [-l LOGFILE] PORT\n"
  "  -a          : Use tcp quick ack on outgoing connections\n"
  "  -A          : Disable tcp quick ack on outgoing connections\n"
  "  -h          : Print help and exit\n"
  "  -l=/dev/null: Duplicate all statements to a logfile\n"
  "  -n          : Use tcp no delay on outgoing connections\n"
  "  -N          : Do not use tcp no delay on outgoing connections\n"
  "  -q          : Quiet printing\n"
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

int respond(int connfd, size_t port, FILE *logfile, int *tcpquickack)
{
  struct setup_header setupBuffer;
  size_t  bytesRead = 0, requestCount = 0, bytesWritten = 0, responseCount = 0, qh = 0, qt = 0;
  ssize_t n;
  uint64_t readEnd, writeEnd;
  struct request_header *requestBuffer;
  struct response_header *responseBuffer;
  struct request *requests;
  fd_set rfds, wfds;

  n = read(connfd, &setupBuffer, sizeof(struct setup_header));
  readEnd = microseconds();
  if (n != sizeof(struct setup_header)) {
    fprintf(stderr, "Failed to read setup from connection on port %lu\n", port);
    return -1;
  }

  write(connfd, &readEnd, sizeof(uint64_t));
  LOGF(logfile, LOG_LEVEL_L, "client %lu sending %lu requests of size %lu expecting responses of size %lu\n", port, setupBuffer.requests, setupBuffer.request_size, setupBuffer.response_size);
  LOGSOCKOPT(logfile, LOG_LEVEL_L, connfd, IPPROTO_TCP, TCP_QUICKACK);
  LOGSOCKOPT(logfile, LOG_LEVEL_L, connfd, IPPROTO_TCP, TCP_NODELAY);
  requestBuffer = malloc(setupBuffer.request_size);
  responseBuffer = malloc(setupBuffer.response_size);
  requests = calloc(setupBuffer.simul + 1, sizeof(struct request));

  memset(responseBuffer, 0xA0, setupBuffer.response_size);

  responseBuffer->prev_seq = 0;
  responseBuffer->prev_index = 0;
  responseBuffer->prev_write_end = microseconds();

  while (!setupBuffer.requests || responseCount < setupBuffer.requests) {
    FD_ZERO(&rfds);
    if (!setupBuffer.requests || requestCount < setupBuffer.requests) {
      FD_SET(connfd, &rfds);
    }

    FD_ZERO(&wfds);
    if (responseCount < requestCount) {
      FD_SET(connfd, &wfds);
    }

    LOGSOCKOPT(logfile, LOG_LEVEL_V, connfd, IPPROTO_TCP, TCP_NODELAY);
    LOGSOCKOPT(logfile, LOG_LEVEL_V, connfd, IPPROTO_TCP, TCP_QUICKACK);
    LOGF(logfile, LOG_LEVEL_V, "selecting requests, %lu requests recieved %lu responses written\n", requestCount, responseCount);
    select(connfd+1, &rfds, &wfds, NULL, NULL);

    if (FD_ISSET(connfd, &rfds)) {
      LOGF(logfile, LOG_LEVEL_V, "reading %ld bytes from port %lu\n", setupBuffer.request_size - bytesRead, port);

      if (bytesRead == 0) {
        requests[qt].request_read_start = microseconds();
      }
      n = read(connfd, ((char*)requestBuffer) + bytesRead, setupBuffer.request_size - bytesRead);
      requests[qt].request_read_end = microseconds();

      if (n < 0) {
        perror("read: ");
        fprintf(stderr, "Failed to read request from connection on port %lu\n", port);
        return -1;
      }
      if (n == 0) {
        fprintf(stderr, "Connection on port %lu closed during read\n", port);
        return -1;
      }

      LOGF(logfile, LOG_LEVEL_V, "read %lu bytes from port %lu\n", n, port);
      bytesRead +=n;

      if (bytesRead == setupBuffer.request_size) {
        requests[qt].seq = requestBuffer->seq;
        requests[qt].index = requestBuffer->index;
        LOGF(logfile, LOG_LEVEL_V, "finished read from port %lu saved %lu, %lu, %lu at %lu to index %lu\n", port, requests[qt].seq, requests[qt].request_read_start, requests[qt].request_read_end, requests[qt].index, qt);
        qt = (qt + 1) % (setupBuffer.simul + 1);
        ++requestCount;
        bytesRead = 0;
      }
    }

    if (FD_ISSET(connfd, &wfds)) {
      if (bytesWritten == 0) {
        responseBuffer->seq = requests[qh].seq;
        responseBuffer->index = requests[qh].index;
        responseBuffer->read_start = requests[qh].request_read_start;
        responseBuffer->read_end = requests[qh].request_read_end;
        responseBuffer->write_start = microseconds();
        LOGF(logfile, LOG_LEVEL_V, "starting write to port %lu for %lu reading from index %lu to index %lu (previous index %lu)\n", port, responseBuffer->seq, qh, responseBuffer->index, responseBuffer->prev_index);
      }
      n = write(connfd, ((char*)responseBuffer) + bytesWritten, setupBuffer.response_size - bytesWritten);
      writeEnd = microseconds();

      SETSOCKOPT(logfile, LOG_LEVEL_V, connfd, IPPROTO_TCP, TCP_QUICKACK, tcpquickack);

      if (n < 0) {
        perror("write: ");
        fprintf(stderr, "Failed to write request to connection on port %lu\n", port);
        return -1;
      }
      if (n == 0) {
        fprintf(stderr, "Connection on port %lu closed during write\n", port);
        return -1;
      }

      LOGF(logfile, LOG_LEVEL_V, "wrote %lu bytes to port %lu\n", n, port);
      bytesWritten += n;

      if (bytesWritten == setupBuffer.response_size) {
        responseBuffer->prev_seq = responseBuffer->seq;
        responseBuffer->prev_index = responseBuffer->index;
        responseBuffer->prev_write_end = writeEnd;
        qh = (qh + 1) % (setupBuffer.simul + 1);
        ++responseCount;
        bytesWritten = 0;
      }
    }
  }
  free(requestBuffer);
  free(responseBuffer);
  free(requests);
  return 0;
}

static int optparse(struct options *options)
{
  size_t i = 0;
  size_t n = 1;

  while (options->argc >= 2 && options->argv[0][0] == '-') {
    switch(options->argv[0][++i]) {
    case 'l': options->logfilename = &options->argv[0][n++]; break;
    case 'a': options->tcpquickack = &option_true; break;
    case 'A': options->tcpquickack = &option_false; break;
    case 'n': options->tcpnodelay = &option_true; break;
    case 'N': options->tcpnodelay = &option_false; break;
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
  char hostaddr[MAXLINE], hostname[MAXLINE], *progname = argv[0], *portstring;
  int listenfd, connfd, error, pid, threaded = 0;
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
    perror(NULL);
    fprintf(stderr, "Warning : Unable to initialize thread mechanisms, child processes may not be cleaned up\n");
  } else {
    threaded = 1;
  }

  memset(&options, 0, sizeof(struct options));
  options.argc = argc - 1;
  options.argv = argv + 1;
  options.log_level = &log_level;

  error = optparse(&options);

  if (error || options.argc != 1) {
    fprintf(stderr, usage, progname);
    return error ? error - 1 : 0;
  }

  if (options.logfilename) {
    logfile = fopen(options.logfilename, "a");
  }

  portstring = options.argv[0];

  listenfd = open_socketfd(NULL, portstring, AI_PASSIVE, &bind);

  if(listenfd < 0) {
    fprintf(stderr, "Error : Cannot listen to socket %s with error %d\n", portstring, listenfd);
    return 1;
  }

  SETSOCKOPT(logfile, LOG_LEVEL_V, listenfd, SOL_SOCKET, SO_REUSEADDR, &option_true);
  SETSOCKOPT(logfile, LOG_LEVEL_L, listenfd, IPPROTO_TCP, TCP_NODELAY, options.tcpnodelay);
  SETSOCKOPT(logfile, LOG_LEVEL_L, listenfd, IPPROTO_TCP, TCP_QUICKACK, options.tcpquickack);

  if (listen(listenfd, LISTEN_MAX) == -1) {
    fprintf(stderr, "Error : Cannot listen on port\n");
    return 1;
  }
  LOG(logfile, LOG_LEVEL_L, "listening\n");

  while(1)
  {
    clientlen = sizeof(clientaddr);
    connfd = accept(listenfd, (void *)(&clientaddr), &clientlen);
    if (connfd == -1) {
      continue;
    }

    LOGSOCKOPT(logfile, LOG_LEVEL_L, connfd, IPPROTO_TCP, TCP_NODELAY);
    LOGSOCKOPT(logfile, LOG_LEVEL_L, connfd, IPPROTO_TCP, TCP_QUICKACK);

    error = getnameinfo((struct sockaddr*)&clientaddr, clientlen, hostname, sizeof(hostname), NULL, 0, 0);
    if (error != 0) {
      close(connfd);
      continue;
    }
    error = getnameinfo((struct sockaddr*)&clientaddr, clientlen, hostaddr, sizeof(hostaddr), NULL, 0, NI_NUMERICHOST);

    port = ntohs(((struct sockaddr*)&clientaddr)->sa_family == AF_INET
         ? ((struct sockaddr_in*)&clientaddr)->sin_port
         : ((struct sockaddr_in6*)&clientaddr)->sin6_port);
    if (error) {
      LOGF(logfile, LOG_LEVEL_L, "connected to %s : %lu\n", hostname, port);
    } else {
      LOGF(logfile, LOG_LEVEL_L, "connected to %s (%s) : %lu\n", hostname, hostaddr, port);
    }

    if ((pid = fork()) == 0) {
      close(listenfd);
      respond(connfd, port, logfile, options.tcpquickack);
      LOGF(logfile, LOG_LEVEL_L, "server: closing connection to %s (%s) : %lu\n", hostname, hostaddr, port);
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
      LOGF(logfile, LOG_LEVEL_L, "server: forked to pid %d, connection accepted\n", pid);
    }
    if(pid == -1) {
      fprintf(stderr, "Error : Failed to fork, connection refused\n");
    }
  }
}
