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

int respond(int connfd, size_t port)
{
  struct setup_header setupBuffer;
  size_t  n, total_bytes = 0, request_count = 0;
  struct request_header * requestBuffer;
  struct response_header * responseBuffer;
  fd_set rfds;

  if (read(connfd, &setupBuffer, sizeof(struct setup_header)) != sizeof(struct setup_header)) {
    fprintf(stderr, "Failed to read setup from connection on port %lu\n", port);
    return -1;
  }

  printf("%lu server: client %lu sending %lu requests of size %lu expecting responses of size %lu\n", microseconds(), port, setupBuffer.requests, setupBuffer.request_size, setupBuffer.response_size);

  requestBuffer = malloc(setupBuffer.request_size);
  responseBuffer = malloc(setupBuffer.response_size);
  responseBuffer->prev_seq = 0;

  while (request_count < setupBuffer.requests) {
    FD_ZERO(&rfds);
    FD_SET(connfd, &rfds);

    select(connfd+1, &rfds, NULL, NULL, NULL);

    if (total_bytes == 0) {
      printf("%lu server: reading next request\n", microseconds());
      responseBuffer->read_start = microseconds();
    }
    printf("%lu server: reading %ld bytes from port %lu\n", microseconds(), setupBuffer.request_size - total_bytes, port);
    n = read(connfd, requestBuffer, setupBuffer.request_size - total_bytes);
    responseBuffer->read_end = microseconds();
    responseBuffer->seq = requestBuffer->seq;
    if (n == 0) {
      fprintf(stderr, "Failed to read request from connection on port %lu\n", port);
      return -1;
    }
    printf("%lu server: read %lu bytes from port %lu\n", microseconds(), n, port);

    total_bytes += n;
    if (total_bytes == setupBuffer.request_size) {
      printf("%lu server: finished reading request\n", microseconds());
      total_bytes = 0;
      responseBuffer->write_start = microseconds();
      n = write(connfd, responseBuffer, setupBuffer.response_size);
      responseBuffer->prev_write_end = microseconds();
      responseBuffer->prev_seq = responseBuffer->seq;
      printf("%lu server: finished writing request\n", microseconds());

      if (n != setupBuffer.response_size) {
        fprintf(stderr, "server: error sending bytes, only sent %lu / %lu\n", n, setupBuffer.response_size);
        return -1;
      }
    }
  }
  return 0;
}

/* driver function */
int main(int argc, char **argv)
{
  /* a semaphore representing the number of running child processes */
  sem_t count;
  /* keeping track of port and sum of bytes sent in a connection */
  size_t port, total = 0;
  /* the thread that cleans up children */
  pthread_t cleaning;
  /*
   * file descriptors for listening and a connection,
   * as well as variables to keep track of forked pid,
   * whether the threading worked, and an argument to setsocketopt
   */
  int listenfd, connfd, error, pid, threaded = 0, optval = 1;

  char tcp_nodelay = 1;
  /* length of client socket addr */
  socklen_t clientlen;
  /* client's socket addr */
  union
  {
    struct sockaddr_in client4;
    struct sockaddr_in6 client6;
  } clientaddr;

  /* buffers for the client's host address and name */
  char hostaddr[MAXLINE], hostname[MAXLINE];
  /* initializes the semaphore and thread */
  if(sem_init(&count, 0, 0) == -1 || pthread_create(&cleaning, NULL, clean, &count) != 0)
    fprintf(stderr, "Warning : Unable to initialize thread mechanisms, child processes may not be cleaned up\n");
  else
    threaded = 1;

  /* opens a socket to listen for connections */
  listenfd = open_socketfd(NULL, argv[1], AI_PASSIVE, &bind);

  if(listenfd < 0)
  {
    fprintf(stderr, "Error : Cannot listen to socket %s with error %d\n", argv[1], listenfd);
    return 1;
  }
  /* sets the socket */
  if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(optval)) == -1)
  {
    fprintf(stderr, "Error : Unable to set socket options\n");
    return 1;
  }
  /* sets up listening */
  if(listen(listenfd, LISTEN_MAX) == -1)
  {
    fprintf(stderr, "Error : Cannot listen on port\n");
    return 1;
  }
  printf("server: listening\n");
  while(1)
  {
    clientlen = sizeof(clientaddr);
    /* accepts a new connection */
    connfd = accept(listenfd, (void *)(&clientaddr), &clientlen);
    if(connfd == -1)
      continue;
    /* gets the name of the client */
    error = getnameinfo((struct sockaddr*)&clientaddr, clientlen, hostname, sizeof(hostname), NULL, 0, 0);
    if(error != 0)
    {
      close(connfd);
      continue;
    }
    /* gets the numeric address of the client */
    error = getnameinfo((struct sockaddr*)&clientaddr, clientlen, hostaddr, sizeof(hostaddr), NULL, 0, NI_NUMERICHOST);

    /*
     * the port needs to be converted from network byte order
     *   decide between ipv4 and ip46
     */
    port = ntohs(((struct sockaddr*)&clientaddr)->sa_family == AF_INET
            ? ((struct sockaddr_in*)&clientaddr)->sin_port
            : ((struct sockaddr_in6*)&clientaddr)->sin6_port);
    if(error == 0)
      printf("server: connected to %s (%s) : %lu\n", hostname, hostaddr, port);
    else
      printf("server: server connected to %s : %lu\n", hostname, port);

    /* forks of a child thread */
    if((pid = fork()) == 0)
    {
      /* child thread */
      close(listenfd);
      /* commences connection */
      setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, (char*)&tcp_nodelay, sizeof(int));
      respond(connfd, port);
      printf("server: closing connection to %s (%s) : %lu\n", hostname, hostaddr, port);
      close(connfd);
      exit(0);
    }
    ++total;
    /* parent thread */
    close(connfd);
    if(pid > 0)
    {
      /* tells the clean up thread there is a new child */
      if(threaded && sem_post(&count) == -1)
        fprintf(stderr, "Warning : Semaphore overflow, child processes may not be cleaned up\n");
      printf("server: forked to pid %d, connection accepted\n", pid);
    }
    if(pid == -1)
      fprintf(stderr, "Error : Failed to fork, connection refused\n");
  }
}
