#include "nartis/cc1101.h"
#include "nartis/nartis_types.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

/* CC1101 driver for a Linux kernel SPI device. Register values match the proven
 * bench driver and the ESPHome component. The important difference from a naive
 * port is the TX framing: hardware preamble+sync are DISABLED on transmit
 * (MDMCFG2.SYNC_MODE = 000) and a clean 0x55 preamble + F6 72 sync are written
 * from the FIFO ahead of the frame. A 16/16 hardware correlator (F6 72) is
 * restored for RX. This removes the 1-bit phase slip that otherwise leaves the
 * meter silent. TX completion is read from MARCSTATE (no GDO required). */

/* Config registers */
enum {
  R_IOCFG2 = 0x00, R_IOCFG1 = 0x01, R_IOCFG0 = 0x02, R_FIFOTHR = 0x03,
  R_SYNC1 = 0x04, R_SYNC0 = 0x05, R_PKTLEN = 0x06, R_PKTCTRL1 = 0x07,
  R_PKTCTRL0 = 0x08, R_ADDR = 0x09, R_CHANNR = 0x0A, R_FSCTRL1 = 0x0B,
  R_FSCTRL0 = 0x0C, R_FREQ2 = 0x0D, R_FREQ1 = 0x0E, R_FREQ0 = 0x0F,
  R_MDMCFG4 = 0x10, R_MDMCFG3 = 0x11, R_MDMCFG2 = 0x12, R_MDMCFG1 = 0x13,
  R_MDMCFG0 = 0x14, R_DEVIATN = 0x15, R_MCSM2 = 0x16, R_MCSM1 = 0x17,
  R_MCSM0 = 0x18, R_FOCCFG = 0x19, R_BSCFG = 0x1A, R_AGCCTRL2 = 0x1B,
  R_AGCCTRL1 = 0x1C, R_AGCCTRL0 = 0x1D, R_WORCTRL = 0x20, R_FREND1 = 0x21,
  R_FREND0 = 0x22, R_FSCAL3 = 0x23, R_FSCAL2 = 0x24, R_FSCAL1 = 0x25,
  R_FSCAL0 = 0x26, R_TEST2 = 0x2C, R_TEST1 = 0x2D, R_TEST0 = 0x2E,
};
/* Status registers (read with burst bit) */
enum {
  S_PARTNUM = 0x30, S_VERSION = 0x31, S_FREQEST = 0x32, S_LQI = 0x33,
  S_RSSI = 0x34, S_MARCSTATE = 0x35,
  S_TXBYTES = 0x3A, S_RXBYTES = 0x3B,
};
enum { R_PATABLE = 0x3E, R_FIFO = 0x3F };
/* Command strobes */
enum {
  CS_SRES = 0x30, CS_SRX = 0x34, CS_STX = 0x35, CS_SIDLE = 0x36,
  CS_SPWD = 0x39, CS_SFRX = 0x3A, CS_SFTX = 0x3B,
};

#define MARC_MASK 0x1F
#define MARC_IDLE 0x01
#define MARC_RX 0x0D
#define RX_HOLDBACK_STALL_MS 30  /* ~9 byte times at 2400 baud */
#define MDMCFG2_BASE 0x12  /* GFSK + 16/16 sync (F6 72) */
#define SYNC_HI 0xF6
#define SYNC_LO 0x72
#define DEFAULT_PREAMBLE 12
#define XTAL_HZ 26000000.0
#define CC1101_FIFO_SIZE 64

static const uint32_t CH_HZ[4] = {433820000u, 433300000u, 434260000u, 434700000u};

struct nartis_cc1101 {
  nartis_backend_t backend;
  nartis_cc1101_config_t cfg;
  int fd;
  int preamble;
  uint8_t rx_channel;
  bool rx_active;
};

static void delay_us(nartis_cc1101_t *d, unsigned us) { d->backend.delay_us(d->backend.ctx, us); }
static uint64_t millis(nartis_cc1101_t *d) { return d->backend.millis(d->backend.ctx); }

static void spi_xfer(nartis_cc1101_t *d, const uint8_t *tx, uint8_t *rx, size_t n) {
  struct spi_ioc_transfer tr;
  memset(&tr, 0, sizeof(tr));
  tr.tx_buf = (unsigned long) tx;
  tr.rx_buf = (unsigned long) rx;  /* NULL (0) => kernel discards received bytes */
  tr.len = (uint32_t) n;
  tr.speed_hz = 4000000;
  tr.bits_per_word = 8;
  (void) ioctl(d->fd, SPI_IOC_MESSAGE(1), &tr);
}

static void wr(nartis_cc1101_t *d, uint8_t addr, uint8_t v) {
  uint8_t t[2] = {addr, v};
  spi_xfer(d, t, NULL, 2);
}
static uint8_t rd_status(nartis_cc1101_t *d, uint8_t addr) {
  uint8_t t[2] = {(uint8_t) (addr | 0xC0), 0}, r[2] = {0, 0};
  spi_xfer(d, t, r, 2);
  return r[1];
}
static void strobe(nartis_cc1101_t *d, uint8_t s) {
  uint8_t t[1] = {s};
  spi_xfer(d, t, NULL, 1);
}
static void wr_burst(nartis_cc1101_t *d, uint8_t addr, const uint8_t *b, size_t n) {
  uint8_t stackbuf[64];
  uint8_t *t = (n + 1 <= sizeof(stackbuf)) ? stackbuf : malloc(n + 1);
  if (!t) return;
  t[0] = (uint8_t) (addr | 0x40);
  memcpy(t + 1, b, n);
  spi_xfer(d, t, NULL, n + 1);
  if (t != stackbuf) free(t);
}
static void rd_burst(nartis_cc1101_t *d, uint8_t addr, uint8_t *b, size_t n) {
  uint8_t stackt[64], stackr[64];
  uint8_t *t = (n + 1 <= sizeof(stackt)) ? stackt : malloc(n + 1);
  uint8_t *r = (n + 1 <= sizeof(stackr)) ? stackr : malloc(n + 1);
  if (!t || !r) { if (t != stackt) free(t); if (r != stackr) free(r); return; }
  memset(t, 0, n + 1);
  t[0] = (uint8_t) (addr | 0xC0);
  spi_xfer(d, t, r, n + 1);
  memcpy(b, r + 1, n);
  if (t != stackt) free(t);
  if (r != stackr) free(r);
}

static uint8_t marcstate(nartis_cc1101_t *d) { return rd_status(d, S_MARCSTATE) & MARC_MASK; }

/* Errata "SPI read synchronization": a status register that changes while it
 * is shifted out can read as a value it never held (classically a bogus
 * RXBYTES). Read until two consecutive reads agree. */
static uint8_t rd_status_stable(nartis_cc1101_t *d, uint8_t addr) {
  uint8_t prev = rd_status(d, addr);
  for (int i = 0; i < 6; ++i) {
    uint8_t cur = rd_status(d, addr);
    if (cur == prev) return cur;
    prev = cur;
  }
  return prev;
}

static int wait_idle(nartis_cc1101_t *d, uint32_t timeout_ms) {
  uint64_t start = millis(d);
  while (millis(d) - start < timeout_ms) {
    if (marcstate(d) == MARC_IDLE) return NARTIS_OK;
    delay_us(d, 200);
  }
  return NARTIS_ERR_TIMEOUT;
}

static void set_freq_hz(nartis_cc1101_t *d, uint32_t carrier_hz) {
  double hz = (double) carrier_hz + (double) d->cfg.freq_offset_hz;
  uint32_t w = (uint32_t) (hz * 65536.0 / XTAL_HZ + 0.5);
  wr(d, R_FREQ2, (uint8_t) ((w >> 16) & 0xFF));
  wr(d, R_FREQ1, (uint8_t) ((w >> 8) & 0xFF));
  wr(d, R_FREQ0, (uint8_t) (w & 0xFF));
}

int nartis_cc1101_open(nartis_cc1101_t **out, const nartis_backend_t *backend,
                       const nartis_cc1101_config_t *cfg) {
  if (!out || !backend) return NARTIS_ERR_ARG;
  nartis_cc1101_t *d = calloc(1, sizeof(*d));
  if (!d) return NARTIS_ERR_IO;
  d->backend = *backend;
  if (cfg) d->cfg = *cfg;
  d->preamble = (d->cfg.tx_preamble_bytes > 0) ? d->cfg.tx_preamble_bytes : DEFAULT_PREAMBLE;
  d->rx_channel = 2;
  const char *dev = d->cfg.spi_dev ? d->cfg.spi_dev : "/dev/spidev0.0";
  d->fd = open(dev, O_RDWR);
  if (d->fd < 0) { free(d); return NARTIS_ERR_IO; }
  uint8_t mode = SPI_MODE_0, bits = 8;
  uint32_t hz = 4000000;
  if (ioctl(d->fd, SPI_IOC_WR_MODE, &mode) < 0 ||
      ioctl(d->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(d->fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
    close(d->fd);
    free(d);
    return NARTIS_ERR_IO;
  }
  *out = d;
  return NARTIS_OK;
}

void nartis_cc1101_close(nartis_cc1101_t *d) {
  if (!d) return;
  if (d->fd >= 0) {
    strobe(d, CS_SIDLE);
    close(d->fd);
  }
  free(d);
}

int nartis_cc1101_init(nartis_cc1101_t *d) {
  if (!d) return NARTIS_ERR_ARG;
  /* Wake from any prior low-power state and reset; retry so a chip left asleep
   * by a previous run reliably comes back. */
  for (int attempt = 0; attempt < 3; ++attempt) {
    strobe(d, CS_SIDLE);  /* wake from SLEEP/XOFF */
    delay_us(d, 1000);
    strobe(d, CS_SRES);
    delay_us(d, 3000);
    strobe(d, CS_SIDLE);
    if (wait_idle(d, 50) == NARTIS_OK) break;
  }

  wr(d, R_IOCFG2, 0x2E);
  wr(d, R_IOCFG1, 0x2E);
  wr(d, R_IOCFG0, 0x06);   /* GDO0: sync sent/received (used only if wired) */
  wr(d, R_FIFOTHR, 0x07);
  wr(d, R_PKTLEN, 0xFF);
  wr(d, R_PKTCTRL1, 0x00);
  wr(d, R_PKTCTRL0, 0x02);  /* infinite length (RX form); TX switches to fixed */
  wr(d, R_ADDR, 0x00);
  wr(d, R_CHANNR, 0x00);
  wr(d, R_FSCTRL1, 0x06);
  wr(d, R_FSCTRL0, 0x00);
  wr(d, R_MDMCFG4, 0xC6);   /* CHANBW 101.56 kHz, DRATE_E=6 */
  wr(d, R_MDMCFG3, 131);    /* DRATE_M -> ~2399 baud */
  wr(d, R_MDMCFG2, MDMCFG2_BASE);
  wr(d, R_MDMCFG1, 0x52);   /* 12 preamble bytes, CHANSPC_E=2 */
  wr(d, R_MDMCFG0, 0xF8);
  wr(d, R_DEVIATN, 0x15);   /* ~5.2 kHz */
  wr(d, R_MCSM2, 0x07);
  wr(d, R_MCSM1, 0x0C);     /* TXOFF -> IDLE */
  wr(d, R_MCSM0, 0x18);     /* FS_AUTOCAL on IDLE->RX/TX */
  wr(d, R_FOCCFG, 0x16);
  wr(d, R_BSCFG, 0x6C);
  wr(d, R_AGCCTRL2, 0x03);
  wr(d, R_AGCCTRL1, 0x40);
  wr(d, R_AGCCTRL0, 0x91);
  wr(d, R_WORCTRL, 0xFB);
  wr(d, R_FREND1, 0x56);
  wr(d, R_FREND0, 0x10);
  wr(d, R_FSCAL3, 0xE9);
  wr(d, R_FSCAL2, 0x2A);
  wr(d, R_FSCAL1, 0x00);
  wr(d, R_FSCAL0, 0x1F);
  wr(d, R_TEST2, 0x88);
  wr(d, R_TEST1, 0x31);
  wr(d, R_TEST0, 0x09);

  uint8_t pa[8] = {0xC0, 0, 0, 0, 0, 0, 0, 0};  /* ~10 dBm at 433 MHz */
  wr_burst(d, R_PATABLE, pa, sizeof(pa));

  set_freq_hz(d, CH_HZ[0]);
  wr(d, R_SYNC1, SYNC_HI);
  wr(d, R_SYNC0, SYNC_LO);
  if (nartis_cc1101_is_connected(d) != NARTIS_OK) {
    nartis_backend_log(&d->backend, NARTIS_LOG_WARN,
                       "CC1101 VERSION=0x%02X unexpected - continuing anyway",
                       rd_status(d, S_VERSION));
  }
  return NARTIS_OK;
}

int nartis_cc1101_is_connected(nartis_cc1101_t *d) {
  uint8_t ver = rd_status(d, S_VERSION);
  return (ver == 0x14 || ver == 0x04 || ver == 0x17) ? NARTIS_OK : NARTIS_ERR_IO;
}

void nartis_cc1101_use_custom_channels(nartis_cc1101_t *d, bool enable) {
  (void) d;
  (void) enable; /* reply carriers are identical in both channel modes */
}

int nartis_cc1101_set_channel(nartis_cc1101_t *d, uint8_t ch) {
  if (!d || ch >= 4) return NARTIS_ERR_ARG;
  d->rx_channel = ch;  /* TX is always ch0; this is the reply carrier */
  return NARTIS_OK;
}

int nartis_cc1101_scan_channels(nartis_cc1101_t *d, int8_t scores[4], uint8_t *best_ch) {
  if (!d) return NARTIS_ERR_ARG;
  int8_t best_score = 127;
  uint8_t best = 2;
  for (uint8_t ch = 0; ch < 4; ++ch) {
    strobe(d, CS_SIDLE);
    wait_idle(d, 20);
    set_freq_hz(d, CH_HZ[ch]);
    wr(d, R_MDMCFG2, MDMCFG2_BASE);
    strobe(d, CS_SFRX);
    strobe(d, CS_SRX);
    delay_us(d, 2000);
    int sum = 0;
    for (int i = 0; i < 8; ++i) {
      delay_us(d, 500);
      int raw = rd_status(d, S_RSSI);
      sum += (raw >= 128) ? ((raw - 256) / 2 - 74) : (raw / 2 - 74);
    }
    int8_t score = (int8_t) (sum / 8);
    if (scores) scores[ch] = score;
    if (ch >= 1 && score < best_score) { best = ch; best_score = score; }
  }
  strobe(d, CS_SIDLE);
  set_freq_hz(d, CH_HZ[0]);
  if (best_ch) *best_ch = best;
  return NARTIS_OK;
}

int nartis_cc1101_transmit(nartis_cc1101_t *d, const uint8_t *data, size_t len) {
  if (!d || !data || len == 0) return NARTIS_ERR_ARG;
  size_t total = (size_t) d->preamble + 2 + len;  /* preamble + F6 72 + frame */
  if (total > 255) return NARTIS_ERR_ARG;          /* fixed-length TX, one FIFO fill */

  strobe(d, CS_SIDLE);
  if (wait_idle(d, 20) != NARTIS_OK) return NARTIS_ERR_TIMEOUT;
  set_freq_hz(d, CH_HZ[0]);  /* CIU always transmits on ch0 */

  /* TX framing fix: disable hardware preamble/sync, emit a clean 0x55 preamble
   * and F6 72 from the FIFO ahead of the frame. */
  wr(d, R_MDMCFG2, (uint8_t) (MDMCFG2_BASE & ~0x07));
  wr(d, R_PKTCTRL0, 0x00);          /* fixed length */
  wr(d, R_PKTLEN, (uint8_t) total);

  uint8_t fifo[255];
  size_t o = 0;
  for (int i = 0; i < d->preamble; ++i) fifo[o++] = 0x55;
  fifo[o++] = SYNC_HI;
  fifo[o++] = SYNC_LO;
  memcpy(fifo + o, data, len);
  o += len;

  /* The TX FIFO holds only 64 bytes; a frame is usually larger, so fill the FIFO,
   * strobe STX, then refill as it drains until the whole frame is written. */
  size_t written = o < CC1101_FIFO_SIZE ? o : CC1101_FIFO_SIZE;
  strobe(d, CS_SFTX);
  wr_burst(d, R_FIFO, fifo, written);
  strobe(d, CS_STX);

  /* Completion = all bytes written and MARCSTATE back to IDLE (MCSM1 TXOFF=IDLE).
   * At ~2400 baud a full frame takes up to ~1 s on air, hence the 2 s budget. */
  uint64_t start = millis(d);
  bool entered_tx = false;
  while (millis(d) - start < 2000) {
    uint8_t m = marcstate(d);
    if (m == 0x16) { strobe(d, CS_SIDLE); strobe(d, CS_SFTX); return NARTIS_ERR_IO; }  /* TX FIFO underflow */
    if (written < o) {
      uint8_t txb = rd_status(d, S_TXBYTES) & 0x7F;
      if (txb < CC1101_FIFO_SIZE - 4) {
        size_t space = CC1101_FIFO_SIZE - txb;
        size_t rem = o - written;
        size_t chunk = rem < space ? rem : space;
        wr_burst(d, R_FIFO, fifo + written, chunk);
        written += chunk;
      }
    }
    if (m == 0x13 || m == 0x14 || m == 0x15) {
      entered_tx = true;
    } else if (entered_tx && written >= o && m == MARC_IDLE) {
      return NARTIS_OK;
    } else if (!entered_tx && (millis(d) - start) > 15 && m == MARC_IDLE) {
      strobe(d, CS_SIDLE);  /* never left IDLE: STX was rejected */
      strobe(d, CS_SFTX);
      return NARTIS_ERR_IO;
    }
    delay_us(d, 100);
  }
  strobe(d, CS_SIDLE);
  strobe(d, CS_SFTX);
  return NARTIS_ERR_TIMEOUT;
}

int nartis_cc1101_start_rx(nartis_cc1101_t *d) {
  if (!d) return NARTIS_ERR_ARG;
  strobe(d, CS_SIDLE);
  wait_idle(d, 20);
  set_freq_hz(d, CH_HZ[d->rx_channel & 3]);
  wr(d, R_MDMCFG2, MDMCFG2_BASE);  /* restore 16/16 sync (F6 72) */
  wr(d, R_PKTCTRL0, 0x02);         /* infinite length; frame length comes from byte 0 */
  wr(d, R_SYNC1, SYNC_HI);
  wr(d, R_SYNC0, SYNC_LO);
  strobe(d, CS_SFRX);
  strobe(d, CS_SRX);
  d->rx_active = true;
  return NARTIS_OK;
}

int nartis_cc1101_poll_rx(nartis_cc1101_t *d, uint8_t *buf, size_t *len_io,
                          size_t max_len, uint32_t timeout_ms) {
  if (!d || !buf || !len_io || !d->rx_active) return NARTIS_ERR_ARG;
  uint64_t start = millis(d);
  size_t got = 0, expected = 0;
  uint8_t last_n = 0;
  uint64_t last_change = start;
  *len_io = 0;
  while (millis(d) - start <= timeout_ms) {
    uint8_t st = rd_status_stable(d, S_RXBYTES);
    if (st & 0x80) {  /* RX FIFO overflow */
      strobe(d, CS_SIDLE);
      strobe(d, CS_SFRX);
      strobe(d, CS_SRX);
      got = 0;
      expected = 0;
      last_n = 0;
      delay_us(d, 500);
      continue;
    }
    uint8_t n = st & 0x7F;
    uint64_t now = millis(d);
    if (n != last_n) { last_n = n; last_change = now; }
    if (n == 0) { delay_us(d, 1000); continue; }
    size_t take = n;
    /* Errata SWRZ020E: emptying the RX FIFO while the demodulator is still
     * writing into it can return a byte that was never received. While in RX,
     * leave the last byte behind (infinite-length mode keeps demodulating, so
     * more bytes arrive behind it); take it only if the count has stalled. */
    if (marcstate(d) == MARC_RX && now - last_change < RX_HOLDBACK_STALL_MS) take = n - 1;
    if (take == 0) { delay_us(d, 1000); continue; }
    if (expected && got + take > expected) take = expected - got;
    if (got + take > max_len) return NARTIS_ERR_PROTOCOL;
    rd_burst(d, R_FIFO, buf + got, take);
    got += take;
    last_n = (uint8_t) (n - take);
    last_change = millis(d);
    if (!expected && got >= 1) {
      expected = (size_t) buf[0] + 1;
      if (expected > max_len) return NARTIS_ERR_PROTOCOL;
    }
    if (expected && got >= expected) {
      *len_io = expected;
      /* Diagnostic: FREQEST = received carrier minus our tuning (~1587 Hz/step). */
      int8_t est = (int8_t) rd_status(d, S_FREQEST);
      uint8_t lqi = rd_status(d, S_LQI);
      int raw = rd_status(d, S_RSSI);
      int rssi = (raw >= 128) ? ((raw - 256) / 2 - 74) : (raw / 2 - 74);
      nartis_backend_log(&d->backend, NARTIS_LOG_DEBUG,
                         "CC1101 RX %zu B: FREQEST=%d (%+ld Hz, freq_offset %+ld) LQI=0x%02X RSSI=%d dBm",
                         expected, (int) est, (long) ((long) est * 26000000L / 16384L),
                         (long) d->cfg.freq_offset_hz, lqi, rssi);
      return NARTIS_OK;
    }
  }
  return NARTIS_ERR_TIMEOUT;
}

int nartis_cc1101_finish_rx(nartis_cc1101_t *d) {
  if (!d) return NARTIS_ERR_ARG;
  d->rx_active = false;
  strobe(d, CS_SIDLE);
  return NARTIS_OK;
}

int nartis_cc1101_go_sleep(nartis_cc1101_t *d) {
  if (!d) return NARTIS_ERR_ARG;
  /* Stay in IDLE rather than full power-down (SPWD): the kernel spidev path
   * cannot do the CSn/CHIP_RDYn wake handshake, so a powered-down chip is not
   * reliably reachable on the next run. IDLE keeps it responsive. */
  strobe(d, CS_SIDLE);
  return wait_idle(d, 20);
}
