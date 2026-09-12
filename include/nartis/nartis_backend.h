#ifndef NARTIS_BACKEND_H
#define NARTIS_BACKEND_H

#include <stdarg.h>
#include <stdint.h>
#include "nartis_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NARTIS_GPIO_INPUT = 0,
  NARTIS_GPIO_OUTPUT = 1,
} nartis_gpio_mode_t;

typedef struct nartis_backend {
  int (*gpio_set_mode)(void *ctx, unsigned line, nartis_gpio_mode_t mode);
  int (*gpio_write)(void *ctx, unsigned line, int value);
  int (*gpio_read)(void *ctx, unsigned line, int *value);
  void (*delay_us)(void *ctx, unsigned usec);
  uint64_t (*millis)(void *ctx);
  uint64_t (*micros)(void *ctx);
  void (*vlog)(void *ctx, nartis_log_level_t level, const char *fmt, va_list ap);
  void *ctx;
} nartis_backend_t;

void nartis_backend_log(const nartis_backend_t *backend, nartis_log_level_t level,
                        const char *fmt, ...);

typedef struct nartis_sysfs_backend nartis_sysfs_backend_t;

int nartis_sysfs_backend_open(nartis_sysfs_backend_t **out);
void nartis_sysfs_backend_close(nartis_sysfs_backend_t *backend);
const nartis_backend_t *nartis_sysfs_backend_iface(nartis_sysfs_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif

