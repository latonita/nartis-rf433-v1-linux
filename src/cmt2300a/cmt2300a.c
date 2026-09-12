#include "nartis/cmt2300a.h"
#include "cmt2300a_defs.h"
#include <stdlib.h>
#include <string.h>

struct nartis_cmt2300a {
  nartis_backend_t backend;
  nartis_cmt2300a_pins_t pins;
  bool initialized;
  bool use_custom_channels;
  bool rx_active;
  size_t rx_expected_len;
  uint64_t rx_tail_wait_ms;
  uint64_t rx_drain_stall_ms;
};

static void delay_us(nartis_cmt2300a_t *d, unsigned us) { d->backend.delay_us(d->backend.ctx, us); }
static uint64_t millis(nartis_cmt2300a_t *d) { return d->backend.millis(d->backend.ctx); }
static uint64_t micros(nartis_cmt2300a_t *d) { return d->backend.micros(d->backend.ctx); }
static int pin_mode(nartis_cmt2300a_t *d, unsigned p, nartis_gpio_mode_t m) { return d->backend.gpio_set_mode(d->backend.ctx, p, m); }
static int pin_write(nartis_cmt2300a_t *d, unsigned p, int v) { return d->backend.gpio_write(d->backend.ctx, p, v ? 1 : 0); }
static int pin_read(nartis_cmt2300a_t *d, unsigned p) { int v = 0; d->backend.gpio_read(d->backend.ctx, p, &v); return v; }
static void spi_delay(nartis_cmt2300a_t *d) { delay_us(d, 1); }
static void sdio_out(nartis_cmt2300a_t *d) { pin_mode(d, d->pins.sdio, NARTIS_GPIO_OUTPUT); }
static void sdio_in(nartis_cmt2300a_t *d) { pin_mode(d, d->pins.sdio, NARTIS_GPIO_INPUT); }

static void spi_send(nartis_cmt2300a_t *d, uint8_t byte) {
  for (int i = 7; i >= 0; --i) {
    pin_write(d, d->pins.sclk, 0);
    pin_write(d, d->pins.sdio, (byte >> i) & 1);
    spi_delay(d);
    pin_write(d, d->pins.sclk, 1);
    spi_delay(d);
  }
  pin_write(d, d->pins.sclk, 0);
}

static uint8_t spi_recv(nartis_cmt2300a_t *d) {
  uint8_t byte = 0;
  for (int i = 7; i >= 0; --i) {
    pin_write(d, d->pins.sclk, 0);
    spi_delay(d);
    pin_write(d, d->pins.sclk, 1);
    spi_delay(d);
    if (pin_read(d, d->pins.sdio)) byte |= (uint8_t)(1u << i);
  }
  pin_write(d, d->pins.sclk, 0);
  return byte;
}

void nartis_cmt2300a_write_reg(nartis_cmt2300a_t *d, uint8_t addr, uint8_t val) {
  pin_write(d, d->pins.csb, 0);
  spi_delay(d);
  sdio_out(d);
  spi_send(d, addr & 0x7F);
  spi_send(d, val);
  pin_write(d, d->pins.csb, 1);
  spi_delay(d);
}

uint8_t nartis_cmt2300a_read_reg(nartis_cmt2300a_t *d, uint8_t addr) {
  pin_write(d, d->pins.csb, 0);
  spi_delay(d);
  sdio_out(d);
  spi_send(d, addr | 0x80);
  sdio_in(d);
  uint8_t v = spi_recv(d);
  pin_write(d, d->pins.csb, 1);
  spi_delay(d);
  return v;
}

static void update_reg(nartis_cmt2300a_t *d, uint8_t addr, uint8_t mask, uint8_t val) {
  uint8_t r = nartis_cmt2300a_read_reg(d, addr);
  nartis_cmt2300a_write_reg(d, addr, (uint8_t)((r & ~mask) | (val & mask)));
}

static void write_bank(nartis_cmt2300a_t *d, uint8_t start, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) nartis_cmt2300a_write_reg(d, (uint8_t)(start + i), data[i]);
}

static void fifo_write(nartis_cmt2300a_t *d, const uint8_t *data, size_t len) {
  sdio_out(d);
  for (size_t i = 0; i < len; ++i) {
    pin_write(d, d->pins.fcsb, 0);
    spi_delay(d);
    spi_send(d, data[i]);
    pin_write(d, d->pins.fcsb, 1);
    delay_us(d, 4);
  }
}

static void fifo_read(nartis_cmt2300a_t *d, uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    pin_write(d, d->pins.fcsb, 0);
    spi_delay(d);
    sdio_in(d);
    data[i] = spi_recv(d);
    pin_write(d, d->pins.fcsb, 1);
    delay_us(d, 4);
  }
}

static void clear_interrupts(nartis_cmt2300a_t *d) {
  nartis_cmt2300a_write_reg(d, REG_INT_CLR1, CLR1_TX_DONE_CLR | CLR1_SL_TMO_CLR | CLR1_RX_TMO_CLR);
  nartis_cmt2300a_write_reg(d, REG_INT_CLR2, CLR2_LBD_CLR | CLR2_PREAM_OK_CLR | CLR2_SYNC_OK_CLR |
                                           CLR2_NODE_OK_CLR | CLR2_CRC_OK_CLR | CLR2_PKT_DONE_CLR);
}

static void clear_fifo(nartis_cmt2300a_t *d) {
  nartis_cmt2300a_write_reg(d, REG_FIFO_CLR, FIFO_CLR_RX | FIFO_CLR_TX);
}

static int wait_for_state(nartis_cmt2300a_t *d, uint8_t target, uint32_t timeout_ms) {
  uint64_t start = micros(d);
  uint64_t timeout = (uint64_t)timeout_ms * 1000u;
  while (micros(d) - start < timeout) {
    if (nartis_cmt2300a_state(d) == target) return NARTIS_OK;
    delay_us(d, STATE_POLL_INTERVAL_US);
  }
  return NARTIS_ERR_TIMEOUT;
}

static void set_tx_payload_len(nartis_cmt2300a_t *d, uint16_t len) {
  nartis_cmt2300a_write_reg(d, REG_PKT14, (uint8_t)(((len >> 8) & 0x07) << 4));
  nartis_cmt2300a_write_reg(d, REG_PKT15, (uint8_t)(len & 0xFF));
}

static void set_int_source(nartis_cmt2300a_t *d, uint8_t source) {
  update_reg(d, REG_INT2_CTL, MASK_INT2_SEL, source & MASK_INT2_SEL);
}

int nartis_cmt2300a_open(nartis_cmt2300a_t **out, const nartis_backend_t *backend,
                         const nartis_cmt2300a_pins_t *pins) {
  if (!out || !backend || !pins) return NARTIS_ERR_ARG;
  nartis_cmt2300a_t *d = calloc(1, sizeof(*d));
  if (!d) return NARTIS_ERR_IO;
  d->backend = *backend;
  d->pins = *pins;
  *out = d;
  return NARTIS_OK;
}

void nartis_cmt2300a_close(nartis_cmt2300a_t *d) { free(d); }

uint8_t nartis_cmt2300a_state(nartis_cmt2300a_t *d) {
  return nartis_cmt2300a_read_reg(d, REG_MODE_STA) & MASK_CHIP_MODE_STA;
}

int nartis_cmt2300a_is_connected(nartis_cmt2300a_t *d) {
  return nartis_cmt2300a_read_reg(d, REG_CMT2) == 0x66 ? NARTIS_OK : NARTIS_ERR_IO;
}

static void write_full_config(nartis_cmt2300a_t *d) {
  write_bank(d, CMT_BANK_ADDR, NARTIS_CMT_BANK, CMT_BANK_SIZE);
  write_bank(d, SYSTEM_BANK_ADDR, NARTIS_SYSTEM_BANK, SYSTEM_BANK_SIZE);
  nartis_cmt2300a_set_channel(d, 0);
  write_bank(d, DATA_RATE_BANK_ADDR, NARTIS_DATA_RATE_BANK, DATA_RATE_BANK_SIZE);
  write_bank(d, BASEBAND_BANK_ADDR, NARTIS_BASEBAND_BANK, BASEBAND_BANK_SIZE);
  write_bank(d, TX_BANK_ADDR, NARTIS_TX_BANK, TX_BANK_SIZE);
  update_reg(d, REG_CMT10, 0x07, 0x02);
}

static void apply_overrides(nartis_cmt2300a_t *d) {
  update_reg(d, REG_SYS2, 0xE0, 0x00);
  nartis_cmt2300a_write_reg(d, REG_PKT10, 0x72);
  nartis_cmt2300a_write_reg(d, REG_PKT11, 0xF6);
  nartis_cmt2300a_write_reg(d, REG_PKT12, 0x55);
  nartis_cmt2300a_write_reg(d, REG_PKT13, 0x55);
  nartis_cmt2300a_write_reg(d, REG_CMT4, 0x1C);
  nartis_cmt2300a_write_reg(d, REG_TX8, 0x56);
  nartis_cmt2300a_write_reg(d, REG_TX9, 0x0B);
  update_reg(d, REG_SYS11, (uint8_t)~MASK_FIFO_MERGE_CFG, FIFO_MERGE_VALUE);
  update_reg(d, REG_FIFO_CTL, MASK_FIFO_MERGE_EN, MASK_FIFO_MERGE_EN);
  update_reg(d, REG_PKT29, MASK_FIFO_TH, FIFO_TH_VALUE);
  update_reg(d, REG_IO_SEL, MASK_GPIO3_SEL, GPIO3_SEL_INT2);
  update_reg(d, REG_INT2_CTL, MASK_INT2_SEL, INT_SEL_RX_FIFO_TH);
  update_reg(d, REG_INT2_CTL, MASK_INT_POLAR, 0x00);
  nartis_cmt2300a_write_reg(d, REG_INT_EN, 0x39);
}

int nartis_cmt2300a_init(nartis_cmt2300a_t *d) {
  if (!d) return NARTIS_ERR_ARG;
  pin_mode(d, d->pins.sclk, NARTIS_GPIO_OUTPUT);
  pin_mode(d, d->pins.csb, NARTIS_GPIO_OUTPUT);
  pin_mode(d, d->pins.fcsb, NARTIS_GPIO_OUTPUT);
  pin_mode(d, d->pins.sdio, NARTIS_GPIO_OUTPUT);
  pin_mode(d, d->pins.gpio3, NARTIS_GPIO_INPUT);
  pin_write(d, d->pins.csb, 1);
  pin_write(d, d->pins.fcsb, 1);
  pin_write(d, d->pins.sclk, 0);

  nartis_cmt2300a_write_reg(d, REG_SOFT_RST, SOFT_RST_VALUE);
  delay_us(d, RESET_DELAY_MS * 1000u);
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_STBY);
  if (wait_for_state(d, STA_STBY, 20) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  int rc = nartis_cmt2300a_is_connected(d);
  if (rc != NARTIS_OK) return rc;
  update_reg(d, REG_MODE_STA, MASK_CFG_RETAIN, MASK_CFG_RETAIN);
  update_reg(d, REG_EN_CTL, 0x20, 0x20);
  write_full_config(d);
  apply_overrides(d);
  clear_interrupts(d);
  clear_fifo(d);
  d->initialized = true;
  return NARTIS_OK;
}

int nartis_cmt2300a_go_standby(nartis_cmt2300a_t *d) {
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_STBY);
  return wait_for_state(d, STA_STBY, STATE_POLL_TIMEOUT_MS);
}

int nartis_cmt2300a_go_sleep(nartis_cmt2300a_t *d) {
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_SLEEP);
  return wait_for_state(d, STA_SLEEP, STATE_POLL_TIMEOUT_MS);
}

int nartis_cmt2300a_go_rx(nartis_cmt2300a_t *d) {
  if (nartis_cmt2300a_state(d) != STA_STBY && nartis_cmt2300a_go_standby(d) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  nartis_cmt2300a_write_reg(d, REG_FIFO_CLR, FIFO_CLR_RX);
  clear_interrupts(d);
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_RFS);
  if (wait_for_state(d, STA_RFS, STATE_POLL_TIMEOUT_MS) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_RX);
  return wait_for_state(d, STA_RX, STATE_POLL_TIMEOUT_MS);
}

int nartis_cmt2300a_go_tx(nartis_cmt2300a_t *d) {
  if (nartis_cmt2300a_state(d) != STA_STBY && nartis_cmt2300a_go_standby(d) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  clear_interrupts(d);
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_TFS);
  if (wait_for_state(d, STA_TFS, STATE_POLL_TIMEOUT_MS) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_TX);
  return wait_for_state(d, STA_TX, STATE_POLL_TIMEOUT_MS);
}

void nartis_cmt2300a_use_custom_channels(nartis_cmt2300a_t *d, bool enable) {
  if (d) d->use_custom_channels = enable;
}

int nartis_cmt2300a_set_channel(nartis_cmt2300a_t *d, uint8_t ch) {
  if (ch >= NUM_CHANNELS) return NARTIS_ERR_ARG;
  if (nartis_cmt2300a_state(d) != STA_STBY) nartis_cmt2300a_go_standby(d);
  const uint8_t (*channels)[8] = d->use_custom_channels ? NARTIS_CUSTOM_CHANNELS : NARTIS_FREQ_CHANNELS;
  write_bank(d, FREQUENCY_BANK_ADDR, channels[ch], FREQUENCY_BANK_SIZE);
  return NARTIS_OK;
}

int nartis_cmt2300a_set_rx_channel(nartis_cmt2300a_t *d, uint8_t ch) {
  if (ch >= NUM_CHANNELS) return NARTIS_ERR_ARG;
  if (nartis_cmt2300a_state(d) != STA_STBY) nartis_cmt2300a_go_standby(d);
  const uint8_t (*channels)[8] = d->use_custom_channels ? NARTIS_CUSTOM_CHANNELS : NARTIS_FREQ_CHANNELS;
  write_bank(d, FREQUENCY_BANK_ADDR, channels[ch], 4);
  return NARTIS_OK;
}

int8_t nartis_cmt2300a_rssi_dbm(nartis_cmt2300a_t *d) {
  return (int8_t)((int)nartis_cmt2300a_read_reg(d, REG_RSSI_DBM) - 128);
}

int nartis_cmt2300a_scan_channels(nartis_cmt2300a_t *d, int8_t out_score[4], uint8_t *best_ch) {
  int8_t best_score = 127;
  uint8_t best = 1;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
    nartis_cmt2300a_go_standby(d);
    nartis_cmt2300a_set_channel(d, ch);
    update_reg(d, REG_SYS11, 0x1F, 0x01);
    nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_RX);
    if (wait_for_state(d, STA_RX, STATE_POLL_TIMEOUT_MS) != NARTIS_OK) {
      if (out_score) out_score[ch] = 127;
      continue;
    }
    delay_us(d, RSSI_SCAN_SAMPLE_DELAY_US);
    int8_t initial = nartis_cmt2300a_rssi_dbm(d);
    int8_t max_seen = initial, min_seen = initial;
    int sum = 0;
    for (uint8_t i = 0; i < RSSI_SCAN_LOOP_SAMPLES; ++i) {
      delay_us(d, RSSI_SCAN_SAMPLE_DELAY_US);
      int8_t s = nartis_cmt2300a_rssi_dbm(d);
      sum += s;
      if (s > max_seen) max_seen = s;
      if (s < min_seen) min_seen = s;
    }
    int8_t score = (int8_t)((sum - max_seen - min_seen) / (RSSI_SCAN_LOOP_SAMPLES - 2));
    if (out_score) out_score[ch] = score;
    if (ch == 1 || (ch > 1 && score < best_score)) {
      best = ch;
      best_score = score;
    }
  }
  nartis_cmt2300a_go_standby(d);
  update_reg(d, REG_SYS11, 0x1F, FIFO_MERGE_VALUE);
  nartis_cmt2300a_set_channel(d, best);
  if (best_ch) *best_ch = best;
  return NARTIS_OK;
}

int nartis_cmt2300a_transmit(nartis_cmt2300a_t *d, const uint8_t *data, size_t len) {
  if (!d || !data || len == 0 || len > NARTIS_MAX_RF_FRAME_SIZE) return NARTIS_ERR_ARG;
  if (nartis_cmt2300a_go_standby(d) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  set_tx_payload_len(d, (uint16_t)len);
  clear_interrupts(d);
  update_reg(d, REG_FIFO_CTL, MASK_FIFO_MERGE_EN | MASK_FIFO_RX_TX_SEL | MASK_SPI_FIFO_RD_WR_SEL,
             MASK_FIFO_MERGE_EN | MASK_FIFO_RX_TX_SEL | MASK_SPI_FIFO_RD_WR_SEL);
  nartis_cmt2300a_write_reg(d, REG_FIFO_CLR, FIFO_RESTORE | FIFO_CLR_RX | FIFO_CLR_TX);
  set_int_source(d, INT_SEL_TX_FIFO_TH);
  size_t initial = len > FIFO_SIZE_MERGED ? FIFO_SIZE_MERGED : len;
  fifo_write(d, data, initial);
  size_t written = initial;
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_TFS);
  if (wait_for_state(d, STA_TFS, STATE_POLL_TIMEOUT_MS) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_TX);
  wait_for_state(d, STA_TX, STATE_POLL_TIMEOUT_MS);
  uint64_t start = millis(d);
  while (millis(d) - start < 600) {
    if (nartis_cmt2300a_read_reg(d, REG_INT_CLR1) & CLR1_TX_DONE_FLG) return NARTIS_OK;
    if (written < len && (nartis_cmt2300a_read_reg(d, REG_FIFO_FLAG) & FIFO_TX_TH) == 0) {
      size_t rem = len - written;
      size_t chunk = rem > TX_REFILL_CHUNK ? TX_REFILL_CHUNK : rem;
      fifo_write(d, data + written, chunk);
      written += chunk;
    }
    delay_us(d, 50);
  }
  nartis_cmt2300a_go_standby(d);
  return NARTIS_ERR_TIMEOUT;
}

int nartis_cmt2300a_start_rx(nartis_cmt2300a_t *d) {
  d->rx_active = true;
  d->rx_expected_len = 0;
  d->rx_tail_wait_ms = 0;
  d->rx_drain_stall_ms = 0;
  nartis_cmt2300a_go_standby(d);
  sdio_in(d);
  update_reg(d, REG_FIFO_CTL, MASK_SPI_FIFO_RD_WR_SEL | MASK_FIFO_RX_TX_SEL, 0x00);
  nartis_cmt2300a_write_reg(d, REG_FIFO_CLR, FIFO_RESTORE | FIFO_CLR_RX | FIFO_CLR_TX);
  clear_interrupts(d);
  set_int_source(d, INT_SEL_RX_FIFO_TH);
  set_tx_payload_len(d, 511);
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_RFS);
  wait_for_state(d, STA_RFS, STATE_POLL_TIMEOUT_MS);
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_RX);
  return wait_for_state(d, STA_RX, STATE_POLL_TIMEOUT_MS);
}

int nartis_cmt2300a_poll_rx(nartis_cmt2300a_t *d, uint8_t *buf, size_t *len_io,
                            size_t max_len, uint32_t timeout_ms) {
  if (!d || !buf || !len_io || !d->rx_active) return NARTIS_ERR_ARG;
  const uint64_t start = millis(d);
  *len_io = 0;
  while (millis(d) - start <= timeout_ms) {
    while (*len_io + FIFO_TH_VALUE <= max_len && pin_read(d, d->pins.gpio3)) {
      fifo_read(d, buf + *len_io, FIFO_TH_VALUE);
      *len_io += FIFO_TH_VALUE;
      d->rx_drain_stall_ms = millis(d);
    }
    if (*len_io >= 1 && d->rx_expected_len == 0) {
      d->rx_expected_len = (size_t)buf[0] + 1;
      if (d->rx_expected_len > max_len) return NARTIS_ERR_PROTOCOL;
    }
    if (d->rx_expected_len && *len_io < d->rx_expected_len) {
      size_t rem = d->rx_expected_len - *len_io;
      if (rem < FIFO_TH_VALUE) {
        if (!d->rx_tail_wait_ms) d->rx_tail_wait_ms = millis(d);
        if (millis(d) - d->rx_tail_wait_ms >= 8) {
          fifo_read(d, buf + *len_io, rem);
          *len_io += rem;
        }
      } else if (d->rx_drain_stall_ms && millis(d) - d->rx_drain_stall_ms >= 20) {
        fifo_read(d, buf + *len_io, rem);
        *len_io += rem;
      }
    }
    if (d->rx_expected_len && *len_io >= d->rx_expected_len) {
      *len_io = d->rx_expected_len;
      return NARTIS_OK;
    }
    delay_us(d, 500);
  }
  return NARTIS_ERR_TIMEOUT;
}

int nartis_cmt2300a_finish_rx(nartis_cmt2300a_t *d) {
  if (!d) return NARTIS_ERR_ARG;
  d->rx_active = false;
  nartis_cmt2300a_go_standby(d);
  clear_interrupts(d);
  return NARTIS_OK;
}

int nartis_cmt2300a_test_gpio3(nartis_cmt2300a_t *d) {
  set_int_source(d, INT_SEL_RX_ACTIVE);
  nartis_cmt2300a_go_standby(d);
  delay_us(d, 300);
  int stby = pin_read(d, d->pins.gpio3);
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_RFS);
  wait_for_state(d, STA_RFS, STATE_POLL_TIMEOUT_MS);
  nartis_cmt2300a_write_reg(d, REG_MODE_CTL, GO_RX);
  wait_for_state(d, STA_RX, STATE_POLL_TIMEOUT_MS);
  delay_us(d, 300);
  int rx = pin_read(d, d->pins.gpio3);
  nartis_cmt2300a_go_standby(d);
  set_int_source(d, INT_SEL_RX_FIFO_TH);
  return (!stby && rx) ? NARTIS_OK : NARTIS_ERR_IO;
}

