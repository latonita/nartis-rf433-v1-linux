#include "nartis/nartis_radio.h"

/* Thin adapter: expose the CC1101 driver through the radio vtable. */

static int cc_init(void *dev) { return nartis_cc1101_init((nartis_cc1101_t *) dev); }
static int cc_test_gpio(void *dev) { (void) dev; return NARTIS_OK; }  /* GDO optional */
static void cc_use_custom(void *dev, bool en) { nartis_cc1101_use_custom_channels((nartis_cc1101_t *) dev, en); }
static int cc_set_channel(void *dev, uint8_t ch) { return nartis_cc1101_set_channel((nartis_cc1101_t *) dev, ch); }
static int cc_scan(void *dev, int8_t s[4], uint8_t *b) { return nartis_cc1101_scan_channels((nartis_cc1101_t *) dev, s, b); }
static int cc_tx(void *dev, const uint8_t *d, size_t n) { return nartis_cc1101_transmit((nartis_cc1101_t *) dev, d, n); }
static int cc_start_rx(void *dev) { return nartis_cc1101_start_rx((nartis_cc1101_t *) dev); }
static int cc_poll_rx(void *dev, uint8_t *b, size_t *l, size_t m, uint32_t t) { return nartis_cc1101_poll_rx((nartis_cc1101_t *) dev, b, l, m, t); }
static int cc_finish_rx(void *dev) { return nartis_cc1101_finish_rx((nartis_cc1101_t *) dev); }
static int cc_sleep(void *dev) { return nartis_cc1101_go_sleep((nartis_cc1101_t *) dev); }
static void cc_close(void *dev) { nartis_cc1101_close((nartis_cc1101_t *) dev); }

static const nartis_radio_ops_t CC1101_OPS = {
  .init = cc_init,
  .test_gpio = cc_test_gpio,
  .use_custom_channels = cc_use_custom,
  .set_channel = cc_set_channel,
  .scan_channels = cc_scan,
  .transmit = cc_tx,
  .start_rx = cc_start_rx,
  .poll_rx = cc_poll_rx,
  .finish_rx = cc_finish_rx,
  .go_sleep = cc_sleep,
  .close = cc_close,
};

int nartis_radio_open_cc1101(nartis_radio_t *out, const nartis_backend_t *backend,
                             const nartis_cc1101_config_t *cfg) {
  if (!out) return NARTIS_ERR_ARG;
  nartis_cc1101_t *dev = NULL;
  int rc = nartis_cc1101_open(&dev, backend, cfg);
  if (rc != NARTIS_OK) return rc;
  out->ops = &CC1101_OPS;
  out->dev = dev;
  return NARTIS_OK;
}
