#ifndef NARTIS_CMT2300A_H
#define NARTIS_CMT2300A_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nartis_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  unsigned sdio;
  unsigned sclk;
  unsigned csb;
  unsigned fcsb;
  unsigned gpio3;
} nartis_cmt2300a_pins_t;

typedef struct nartis_cmt2300a nartis_cmt2300a_t;

int nartis_cmt2300a_open(nartis_cmt2300a_t **out, const nartis_backend_t *backend,
                         const nartis_cmt2300a_pins_t *pins);
void nartis_cmt2300a_close(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_init(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_is_connected(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_test_gpio3(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_go_standby(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_go_sleep(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_go_rx(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_go_tx(nartis_cmt2300a_t *dev);
uint8_t nartis_cmt2300a_state(nartis_cmt2300a_t *dev);
void nartis_cmt2300a_use_custom_channels(nartis_cmt2300a_t *dev, bool enable);
int nartis_cmt2300a_set_channel(nartis_cmt2300a_t *dev, uint8_t ch);
int nartis_cmt2300a_set_rx_channel(nartis_cmt2300a_t *dev, uint8_t ch);
int nartis_cmt2300a_scan_channels(nartis_cmt2300a_t *dev, int8_t out_score[4],
                                  uint8_t *best_ch);
int nartis_cmt2300a_transmit(nartis_cmt2300a_t *dev, const uint8_t *data, size_t len);
int nartis_cmt2300a_start_rx(nartis_cmt2300a_t *dev);
int nartis_cmt2300a_poll_rx(nartis_cmt2300a_t *dev, uint8_t *buf, size_t *len_io,
                            size_t max_len, uint32_t timeout_ms);
int nartis_cmt2300a_finish_rx(nartis_cmt2300a_t *dev);
int8_t nartis_cmt2300a_rssi_dbm(nartis_cmt2300a_t *dev);
uint8_t nartis_cmt2300a_read_reg(nartis_cmt2300a_t *dev, uint8_t addr);
void nartis_cmt2300a_write_reg(nartis_cmt2300a_t *dev, uint8_t addr, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif

