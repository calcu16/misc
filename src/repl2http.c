#define MAIN

#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "traffic-shared.h"
char* app_type = "server";

typedef int (*write_func)(int fd, const char* data, int count);

int writeall(int fd, const char* data, int count) {
  int n;
  while (count > 0) {
    n = write(fd, data, count);
    if (n < 0) {
      return n;
    }
    data += n;
    count -= n;
  }
  return 0;
}

int decode_and_write(int fd, const char* data, int count) {
  char buffer[4096];
  int d, i, r, x = 0; 
  while (count > 0) {
    for (i = 0; count > 0 && i < sizeof(buffer); ++i) {
      if (*data == '%') {
        sscanf(data + 1, "%02x", &d);
        buffer[i] = (char) d;
        data += 3;
        count -= 3;
      } else {
        buffer[i] = *data;
        ++data;
        --count;
      }
    }
    r = writeall(fd, buffer, i);
    if (r <= 0) {
      return r;
    }
    x += r;
  }
  return x;
}

int writechunk(int fd, const char* data, int count) {
  int n;
  n = dprintf(fd, "%x\r\n", count);
  if (n < 0) {
    return n;
  }
  n = writeall(fd, data, count);
  if (n < 0) {
    return n;
  }
  n = dprintf(fd, "\r\n");
  if (n < 0) {
    return n;
  }
  return count;
}

struct read_stream {
  char rbuf[4096];
  int rfd;
  int rn;
  int ri;
};

void init(struct read_stream* rs, int fd) {
  rs->rfd = fd;
  rs->rn = rs->ri = 0;
}

int skip_past(struct read_stream* rs, const char* str) {
  int k = 0;
  for (;;) {
    for (;str[k] && rs->ri < rs->rn; ++rs->ri) {
      if (str[k] == rs->rbuf[rs->ri]) {
        ++k;
      } else if (str[0] == rs->rbuf[rs->ri]) {
        // technically its possible to restart in the middle, not implemented here
        k = 1;
      } else {
        k = 0;
      }
    }
    if (!str[k]) {
      return 1;
    }
    rs->rn = read(rs->rfd, rs->rbuf, sizeof(rs->rbuf));
    if (rs->rn <= 0) {
      return rs->rn;
    }
    rs->ri = 0;
  }
}

int write_until(struct read_stream* rs, const char* str, write_func func, int wfd, char needs_newline) {
  int s, k = 0, nl = 1;
  for (;;) {
    for (s = rs->ri; str[k] && rs->ri < rs->rn; ++rs->ri) {
      if (!nl && k == 0) {
        // do nothing
      } else if (str[k] == rs->rbuf[rs->ri]) {
        ++k;
      } else {
        if (k > rs->ri) {
          (*func)(wfd, str, k - rs->ri); 
        }
        k = 0;
     }
     nl = (rs->rbuf[rs->ri] == '\n') || !needs_newline;
    }
    if (!str[k]) {
      if (rs->ri > k + s) {
        (*func)(wfd, rs->rbuf + s, rs->ri - k - s);
      }
      return 1;
    }
    if (rs->ri > k + s) {
       writechunk(wfd, rs->rbuf + s, rs->ri - k - s);
    }
    rs->rn = read(rs->rfd, rs->rbuf, sizeof(rs->rbuf));
    if (rs->rn <= 0) {
      return rs->rn;
    }
    rs->ri = 0;
  }
}

int main(int argc, char** argv) {
  static int option_true = 1;
  struct read_stream http, repl;
  int repl_in[2], repl_out[2];
  int socketfd, connfd;
  int ppid = getpid();
  
  if (argc < 4) {
    fprintf(stderr, "%s [PORT] [PROMPT] [CMD...]\n", argv[0]);
    return 1;
  }

  if (!argv[2][0]) {
    fprintf(stderr, "prompt must be nonempty\n");
    return 1;
  }

  if (strchr(argv[2], '\n')) {
    fprintf(stderr, "prompt cannot contain a newline\n");
    return 1;
  }


  if (pipe(repl_in) == -1) {
    perror("pipe");
    return 1;
  }

  if (pipe(repl_out) == -1) {
    perror("pipe");
    return 1;
  }


  int pid = fork();
  if (pid == -1) {
    perror("fork");
    return 1;
  }
  
  if (pid == 0) {
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
      perror("prctl");
      return 1;
    }

    if (getppid() != ppid) {
      fprintf(stderr, "dead parent\n");
      return 1;
    }

    dup2(repl_in[0], STDIN_FILENO);
    dup2(repl_out[1], STDOUT_FILENO);

    close(repl_in[0]);
    close(repl_in[1]);
    close(repl_out[0]);
    close(repl_out[1]);

    execvp(argv[3], argv + 3);
    perror("exec");
    return 1;
  }

  close(repl_in[0]);
  close(repl_out[1]);
  init(&repl, repl_out[0]);

  socketfd = open_socketfd(NULL, argv[1], AI_PASSIVE, SOCK_STREAM, &bind);
  if(socketfd < 0) {
    fprintf(stderr, "Error : Cannot listen to socket %s\n", argv[1]);
    return 1;
  }
  setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &option_true, sizeof(int));
  setsockopt(socketfd, IPPROTO_TCP, TCP_NODELAY, &option_true, sizeof(int));
  setsockopt(socketfd, IPPROTO_TCP, TCP_QUICKACK, &option_true, sizeof(int));


  if (listen(socketfd, 0) == -1) {
    perror("listen");
    return 1;
  }

  if (skip_past(&repl, argv[2]) <= 0) {
     perror("read");
     return 1;
  }

  for (;;) {
    connfd = accept(socketfd, NULL, NULL);
    if (connfd == -1) {
      continue;
    }
    init(&http, connfd);
    for (;;) {
      if (skip_past(&http, "GET /") <= 0) break;
      if (write_until(&http, " ", &decode_and_write, repl_in[1], 0) <= 0) break;
      if (dprintf(repl_in[1], "\n") <= 0) break;
      if (skip_past(&http, "\r\n\r\n") <= 0) break;
    
      if (dprintf(connfd, "HTTP/1.1 200 GOOD\r\nTransfer-Encoding: chunked\r\n\r\n") <= 0) break;
      if (write_until(&repl, argv[2], &writechunk, connfd, 1) <= 0) break;
      if (dprintf(connfd, "0\r\n\r\n") <= 0) break;
    }
    close(connfd);
  }
  printf("exiting\n");
  return 0;
}
