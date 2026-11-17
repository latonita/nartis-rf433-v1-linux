#ifndef NARTIS_RADIO_H
#define NARTIS_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cmt2300a.h"
#include "cc1101.h"
#include "nartis_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Radio abstraction: the protocol/session layers talk to a radio through this
 * table, so CMT2300A and CC1101 are interchangeable. Mirrors the RadioHal of
 * the ESPHome component. The frame passed to transmit() starts at the length
 * byte (sync/preamble are the radio's job); poll_rx() returns the same form. */
typedef struct nartis_radio_ops {
  int (*init)(void *dev);
  int (*test_gpio)(void *dev);
  void (*use_custom_channels)(void *dev, bool enable);
  int (*set_channel)(void *dev, uint8_t ch);
  int (*scan_channels)(void *dev, int8_t scores[4], uint8_t *best_ch);
  int (*transmit)(void *dev, const uint8_t *data, size_t len);
  int (*start_rx)(void *dev);
  int (*poll_rx)(void *dev, uint8_t *buf, size_t *len_io, size_t max_len, uint32_t timeout_ms);
  int (*finish_rx)(void *dev);
  int (*go_sleep)(void *dev);
  void (*close)(void *dev);
} nartis_radio_ops_t;

typedef struct {
  const nartis_radio_ops_t *ops;
  void *dev;
} nartis_radio_t;

typedef enum {
  NARTIS_RADIO_CMT2300A = 0,
  NARTIS_RADIO_CC1101 = 1,
} nartis_radio_type_t;

int nartis_radio_open_cmt2300a(nartis_radio_t *out, const nartis_backend_t *backend,
                               const nartis_cmt2300a_pins_t *pins);
int nartis_radio_open_cc1101(nartis_radio_t *out, const nartis_backend_t *backend,
                             const nartis_cc1101_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif
