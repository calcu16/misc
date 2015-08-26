#define MAIN
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define LENGTHOF(a) (sizeof(a)/sizeof(*(a)))
static const char usage[] = "usage: %s PROG_NAME SAMPLE FABRIC HOST CLIENT_SERVICE DIRECTION SERVER_SERVICE\n";
static const char fmt[] = "%*lu client: seq %*lu: %ld %*lu %*lu %ld %ld %*lu %*lu %ld +/- %ld %ld";

int cmp(const void *ap, const void *bp) {
  uint64_t a = *(const int64_t*)ap, b = *(const int64_t*)bp;
  return a < b ? -1 :
         a > b ?  1 : 0;
}

void submit(char * prog, char* fabric, char* metric, char* host, char* value) {
  if (!fork()) {
    execlp(prog, prog, "--fabric", fabric, "send_metric", "--appname", "traffic", "--metric_name", metric, "--host", host, "--val", value, "--metric_type", "GAUGE", NULL);
    _exit(1);
  } else {
    wait(NULL);
  }
}

enum {
  PROG_NAME,
  SUBMIT_PROG,
  SAMPLE,
  FABRIC,
  CLIENT_HOST,
  CLIENT_SERVICE,
  DIRECTION,
  SERVER_SERVICE,
  LENGTH
};

int PERCENTILES[] = { 50, 75, 90, 95, 99 };

int main(int argc, char** argv) {
  if (argc != LENGTH) {
    fprintf(stderr, usage, argv[0]);
    return 1;
  }

  int64_t *out_buffer, *in_buffer, sample = atoll(argv[SAMPLE]);
  int64_t out_start, out_end, in_start, in_end, lower_delta, upper_delta, i;
  char * submit_prog = argv[SUBMIT_PROG];
  char *fabric = argv[FABRIC], *client_host = argv[CLIENT_HOST], *client_service = argv[CLIENT_SERVICE];
  char *direction = argv[DIRECTION], *server_service = argv[SERVER_SERVICE];
  char metric_buffer[256], value_buffer[256];
  int n, index[LENGTHOF(PERCENTILES)];
  double sum;
  int64_t max;

  for (i = 0; i < LENGTHOF(index); ++i) {
    index[i] = PERCENTILES[i] * sample / 100;
  }

  out_buffer = malloc(sample * sizeof(uint64_t));
  in_buffer = malloc(sample * sizeof(uint64_t));

  while (1) {
    for (i = 0; i < sample; ++i) {
      n = scanf(fmt, &out_start, &out_end, &in_start, &in_end, &lower_delta, &upper_delta, i);
      if (n == EOF) {
        return 0;
      }
      out_buffer[i] = (out_end + upper_delta < out_start) ? 0 : out_end - out_start + upper_delta;
      in_buffer[i] = (in_end < in_start + lower_delta) ? 0 : in_end - in_start - lower_delta;
    }
    qsort(out_buffer, sample, sizeof(int64_t), cmp);
    qsort(in_buffer, sample, sizeof(int64_t), cmp);

    for (i = 0; i < LENGTHOF(index); ++i) {
      sprintf(metric_buffer, "client_%s_%s_%s_lat%dth", client_service, direction, server_service, PERCENTILES[i]);
      sprintf(value_buffer, "%ld", out_buffer[index[i]]);
      submit(submit_prog, fabric, metric_buffer, client_host, value_buffer);

      sprintf(metric_buffer, "server_%s_%s_%s_lat%dth", server_service, direction, client_service, PERCENTILES[i]);
      sprintf(value_buffer, "%ld", in_buffer[index[i]]);
      submit(submit_prog, fabric, metric_buffer, client_host, value_buffer);
    }

    for (sum = max = i = 0; i < sample; ++i) {
      sum += out_buffer[i];
      if (max < out_buffer[i]) {
        max = out_buffer[i];
      }
    }
    sprintf(metric_buffer, "client_%s_%s_%s_latmax", client_service, direction, server_service);
    sprintf(value_buffer, "%ld", max);
    submit(submit_prog, fabric, metric_buffer, client_host, value_buffer);

    sprintf(metric_buffer, "client_%s_%s_%s_latavg", client_service, direction, server_service);
    sprintf(value_buffer, "%f", sum / sample);
    submit(submit_prog, fabric, metric_buffer, client_host, value_buffer);

    for (sum = max = i = 0; i < sample; ++i) {
      sum += in_buffer[i];
      if (max < in_buffer[i]) {
        max = in_buffer[i];
      }
    }
    sprintf(metric_buffer, "server_%s_%s_%s_latmax", server_service, direction, client_service);
    sprintf(value_buffer, "%ld", max);
    submit(submit_prog, fabric, metric_buffer, client_host, value_buffer);

    sprintf(metric_buffer, "server_%s_%s_%s_latavg", server_service, direction, client_service);
    sprintf(value_buffer, "%f", sum / sample);
    submit(submit_prog, fabric, metric_buffer, client_host, value_buffer);
  }
  free(out_buffer);
  free(in_buffer);
}
