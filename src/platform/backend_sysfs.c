#include "nartis/nartis_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct nartis_sysfs_backend {
  nartis_backend_t iface;
};

static int write_text(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  ssize_t n = write(fd, text, strlen(text));
  int e = errno;
  close(fd);
  return n < 0 ? -e : 0;
}

static int export_gpio(unsigned line) {
  char path[96];
  snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u", line);
  if (access(path, F_OK) == 0) return 0;
  char num[16];
  snprintf(num, sizeof(num), "%u", line);
  int rc = write_text("/sys/class/gpio/export", num);
  if (rc == -EBUSY) return 0;
  if (rc) return rc;
  for (int i = 0; i < 50; ++i) {
    if (access(path, F_OK) == 0) return 0;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
    nanosleep(&ts, NULL);
  }
  return -ETIMEDOUT;
}

static int sysfs_gpio_set_mode(void *ctx, unsigned line, nartis_gpio_mode_t mode) {
  (void)ctx;
  int rc = export_gpio(line);
  if (rc) return rc;
  char path[96];
  snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", line);
  return write_text(path, mode == NARTIS_GPIO_OUTPUT ? "out" : "in");
}

static int sysfs_gpio_write(void *ctx, unsigned line, int value) {
  (void)ctx;
  char path[96];
  snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", line);
  return write_text(path, value ? "1" : "0");
}

static int sysfs_gpio_read(void *ctx, unsigned line, int *value) {
  (void)ctx;
  char path[96];
  snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", line);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  char c = '0';
  ssize_t n = read(fd, &c, 1);
  int e = errno;
  close(fd);
  if (n < 0) return -e;
  *value = c == '1';
  return 0;
}

static void sysfs_delay_us(void *ctx, unsigned usec) {
  (void)ctx;
  struct timespec ts;
  ts.tv_sec = usec / 1000000u;
  ts.tv_nsec = (long)(usec % 1000000u) * 1000L;
  nanosleep(&ts, NULL);
}

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t sysfs_millis(void *ctx) {
  (void)ctx;
  return monotonic_ns() / 1000000ull;
}

static uint64_t sysfs_micros(void *ctx) {
  (void)ctx;
  return monotonic_ns() / 1000ull;
}

static void sysfs_vlog(void *ctx, nartis_log_level_t level, const char *fmt, va_list ap) {
  (void)ctx;
  static const char *names[] = {"ERR", "WRN", "INF", "DBG", "TRC"};
  fprintf(stderr, "[%s] ", names[level >= 0 && level <= NARTIS_LOG_TRACE ? level : 0]);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

int nartis_sysfs_backend_open(nartis_sysfs_backend_t **out) {
  if (!out) return NARTIS_ERR_ARG;
  nartis_sysfs_backend_t *b = calloc(1, sizeof(*b));
  if (!b) return NARTIS_ERR_IO;
  b->iface.gpio_set_mode = sysfs_gpio_set_mode;
  b->iface.gpio_write = sysfs_gpio_write;
  b->iface.gpio_read = sysfs_gpio_read;
  b->iface.delay_us = sysfs_delay_us;
  b->iface.millis = sysfs_millis;
  b->iface.micros = sysfs_micros;
  b->iface.vlog = sysfs_vlog;
  b->iface.ctx = b;
  *out = b;
  return NARTIS_OK;
}

void nartis_sysfs_backend_close(nartis_sysfs_backend_t *backend) {
  free(backend);
}

const nartis_backend_t *nartis_sysfs_backend_iface(nartis_sysfs_backend_t *backend) {
  return backend ? &backend->iface : NULL;
}

