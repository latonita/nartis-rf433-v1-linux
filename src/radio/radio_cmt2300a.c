#include "nartis/nartis_radio.h"

/* Thin adapter: expose the CMT2300A driver through the radio vtable. */

static int cmt_init(void *dev) { return nartis_cmt2300a_init((nartis_cmt2300a_t *) dev); }
static int cmt_test_gpio(void *dev) { return nartis_cmt2300a_test_gpio3((nartis_cmt2300a_t *) dev); }
static void cmt_use_custom(void *dev, bool en) { nartis_cmt2300a_use_custom_channels((nartis_cmt2300a_t *) dev, en); }
static int cmt_set_channel(void *dev, uint8_t ch) { return nartis_cmt2300a_set_channel((nartis_cmt2300a_t *) dev, ch); }
static int cmt_scan(void *dev, int8_t s[4], uint8_t *b) { return nartis_cmt2300a_scan_channels((nartis_cmt2300a_t *) dev, s, b); }
static int cmt_tx(void *dev, const uint8_t *d, size_t n) { return nartis_cmt2300a_transmit((nartis_cmt2300a_t *) dev, d, n); }
static int cmt_start_rx(void *dev) { return nartis_cmt2300a_start_rx((nartis_cmt2300a_t *) dev); }
static int cmt_poll_rx(void *dev, uint8_t *b, size_t *l, size_t m, uint32_t t) { return nartis_cmt2300a_poll_rx((nartis_cmt2300a_t *) dev, b, l, m, t); }
static int cmt_finish_rx(void *dev) { return nartis_cmt2300a_finish_rx((nartis_cmt2300a_t *) dev); }
static int cmt_sleep(void *dev) { return nartis_cmt2300a_go_sleep((nartis_cmt2300a_t *) dev); }
static void cmt_close(void *dev) { nartis_cmt2300a_close((nartis_cmt2300a_t *) dev); }

static const nartis_radio_ops_t CMT_OPS = {
  .init = cmt_init,
  .test_gpio = cmt_test_gpio,
  .use_custom_channels = cmt_use_custom,
  .set_channel = cmt_set_channel,
  .scan_channels = cmt_scan,
  .transmit = cmt_tx,
  .start_rx = cmt_start_rx,
  .poll_rx = cmt_poll_rx,
  .finish_rx = cmt_finish_rx,
  .go_sleep = cmt_sleep,
  .close = cmt_close,
};

int nartis_radio_open_cmt2300a(nartis_radio_t *out, const nartis_backend_t *backend,
                               const nartis_cmt2300a_pins_t *pins) {
  if (!out) return NARTIS_ERR_ARG;
  nartis_cmt2300a_t *dev = NULL;
  int rc = nartis_cmt2300a_open(&dev, backend, pins);
  if (rc != NARTIS_OK) return rc;
  out->ops = &CMT_OPS;
  out->dev = dev;
  return NARTIS_OK;
}
