#ifndef NARTIS_CC1101_H
#define NARTIS_CC1101_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nartis_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CC1101 on a Linux kernel SPI device (e.g. /dev/spidev0.0). Unlike the
 * CMT2300A driver, the CC1101 uses the kernel spidev interface directly for
 * data, and only borrows the backend for timing and logging. GDO0 is optional:
 * without it the driver polls RXBYTES over SPI. */
typedef struct {
  const char *spi_dev;        /* "/dev/spidev0.0" if NULL */
  int gdo0;                   /* backend GPIO line for GDO0, or <0 to poll over SPI */
  int freq_offset_hz;         /* signed crystal correction, applied to TX and RX */
  int tx_preamble_bytes;      /* 0x55 bytes before F6 72 on TX, 0 => default (12) */
} nartis_cc1101_config_t;

typedef struct nartis_cc1101 nartis_cc1101_t;

int nartis_cc1101_open(nartis_cc1101_t **out, const nartis_backend_t *backend,
                       const nartis_cc1101_config_t *cfg);
void nartis_cc1101_close(nartis_cc1101_t *dev);
int nartis_cc1101_init(nartis_cc1101_t *dev);
int nartis_cc1101_is_connected(nartis_cc1101_t *dev);
void nartis_cc1101_use_custom_channels(nartis_cc1101_t *dev, bool enable);
int nartis_cc1101_set_channel(nartis_cc1101_t *dev, uint8_t ch);
int nartis_cc1101_scan_channels(nartis_cc1101_t *dev, int8_t scores[4], uint8_t *best_ch);
int nartis_cc1101_transmit(nartis_cc1101_t *dev, const uint8_t *data, size_t len);
int nartis_cc1101_start_rx(nartis_cc1101_t *dev);
int nartis_cc1101_poll_rx(nartis_cc1101_t *dev, uint8_t *buf, size_t *len_io,
                          size_t max_len, uint32_t timeout_ms);
int nartis_cc1101_finish_rx(nartis_cc1101_t *dev);
int nartis_cc1101_go_sleep(nartis_cc1101_t *dev);

#ifdef __cplusplus
}
#endif

#endif
