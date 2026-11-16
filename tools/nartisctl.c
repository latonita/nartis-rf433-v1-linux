#include "nartis/nartis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f) {
  fprintf(f,
    "Usage:\n"
    "  nartisctl chip-id --sdio N --sclk N --csb N --fcsb N --gpio3 N\n"
    "  nartisctl scan    --meter-serial S --sdio N --sclk N --csb N --fcsb N --gpio3 N\n"
    "  nartisctl pair    --meter-serial S --session PATH --sdio N --sclk N --csb N --fcsb N --gpio3 N\n"
    "  nartisctl read    --meter-serial S --session PATH --obis A.B.C.D.E.F[,class=N,attr=N] ... pins...\n"
    "\n"
    "Options: --ciu-serial S --ciu-address HEX16 --fixed-channel N --alternative-channels --batch-size N\n"
    "\n"
    "Radio (default CMT2300A, bit-banged SPI via --sdio/--sclk/--csb/--fcsb/--gpio3):\n"
    "  --radio cc1101            use a CC1101 on a kernel SPI device instead\n"
    "  --spi-dev PATH            CC1101 SPI device (default /dev/spidev0.0)\n"
    "  --gdo0 N                  CC1101 GDO0 GPIO (optional; polled over SPI if omitted)\n"
    "  --freq-offset HZ          CC1101 signed crystal correction, applied to TX and RX\n"
    "  --tx-preamble N           CC1101 0x55 preamble bytes before sync (default 12)\n");
}

static int parse_u(const char *s) { return s ? (int)strtol(s, NULL, 0) : -1; }

static const char *argval(int *i, int argc, char **argv) {
  if (*i + 1 >= argc) return NULL;
  (*i)++;
  return argv[*i];
}

static int parse_attr(const char *s, nartis_attr_t *a) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", s);
  char *save = NULL;
  char *tok = strtok_r(buf, ",", &save);
  if (!tok || nartis_obis_parse(tok, &a->obis) != NARTIS_OK) return NARTIS_ERR_ARG;
  a->class_id = 3;
  a->attr_id = 2;
  while ((tok = strtok_r(NULL, ",", &save))) {
    if (strncmp(tok, "class=", 6) == 0) a->class_id = (uint16_t)strtoul(tok + 6, NULL, 0);
    else if (strncmp(tok, "attr=", 5) == 0) a->attr_id = (uint8_t)strtoul(tok + 5, NULL, 0);
  }
  return NARTIS_OK;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  const char *cmd = argv[1];
  nartis_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.fixed_channel = -1;
  cfg.batch_size = 5;
  cfg.rx_timeout_ms = 3000;
  cfg.rx_reply_timeout_ms = 900;
  cfg.radio_type = NARTIS_RADIO_CMT2300A;
  cfg.cc1101.gdo0 = -1;
  nartis_attr_t attrs[32];
  size_t attr_count = 0;

  for (int i = 2; i < argc; ++i) {
    const char *a = argv[i];
    if (strcmp(a, "--sdio") == 0) cfg.pins.sdio = (unsigned)parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--sclk") == 0) cfg.pins.sclk = (unsigned)parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--csb") == 0) cfg.pins.csb = (unsigned)parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--fcsb") == 0) cfg.pins.fcsb = (unsigned)parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--gpio3") == 0) cfg.pins.gpio3 = (unsigned)parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--meter-serial") == 0) cfg.meter_serial = argval(&i, argc, argv);
    else if (strcmp(a, "--ciu-serial") == 0) cfg.ciu_serial = argval(&i, argc, argv);
    else if (strcmp(a, "--ciu-address") == 0) cfg.ciu_address_hex = argval(&i, argc, argv);
    else if (strcmp(a, "--session") == 0) cfg.session_path = argval(&i, argc, argv);
    else if (strcmp(a, "--fixed-channel") == 0) cfg.fixed_channel = parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--alternative-channels") == 0) cfg.use_alternative_channels = true;
    else if (strcmp(a, "--batch-size") == 0) cfg.batch_size = (uint8_t)parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--radio") == 0) {
      const char *v = argval(&i, argc, argv);
      if (v && strcmp(v, "cc1101") == 0) cfg.radio_type = NARTIS_RADIO_CC1101;
      else if (v && strcmp(v, "cmt2300a") == 0) cfg.radio_type = NARTIS_RADIO_CMT2300A;
      else { fprintf(stderr, "--radio must be cmt2300a or cc1101\n"); return 2; }
    }
    else if (strcmp(a, "--spi-dev") == 0) cfg.cc1101.spi_dev = argval(&i, argc, argv);
    else if (strcmp(a, "--gdo0") == 0) cfg.cc1101.gdo0 = parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--freq-offset") == 0) cfg.cc1101.freq_offset_hz = parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--tx-preamble") == 0) cfg.cc1101.tx_preamble_bytes = parse_u(argval(&i, argc, argv));
    else if (strcmp(a, "--obis") == 0) {
      const char *v = argval(&i, argc, argv);
      if (!v || attr_count >= 32 || parse_attr(v, &attrs[attr_count]) != NARTIS_OK) {
        fprintf(stderr, "bad --obis value\n");
        return 2;
      }
      attr_count++;
    } else {
      fprintf(stderr, "unknown option: %s\n", a);
      usage(stderr);
      return 2;
    }
  }

  if (cfg.radio_type == NARTIS_RADIO_CMT2300A &&
      !cfg.pins.sdio && !cfg.pins.sclk && !cfg.pins.csb && !cfg.pins.fcsb && !cfg.pins.gpio3) {
    fprintf(stderr, "GPIO pins are required\n");
    return 2;
  }
  if (strcmp(cmd, "chip-id") != 0 && !cfg.meter_serial) {
    fprintf(stderr, "--meter-serial is required\n");
    return 2;
  }

  nartis_sysfs_backend_t *sysfs = NULL;
  int rc = nartis_sysfs_backend_open(&sysfs);
  if (rc != NARTIS_OK) {
    fprintf(stderr, "backend: %s\n", nartis_strerror(rc));
    return 1;
  }
  const nartis_backend_t *backend = nartis_sysfs_backend_iface(sysfs);

  if (strcmp(cmd, "chip-id") == 0) {
    if (cfg.radio_type == NARTIS_RADIO_CC1101) {
      nartis_cc1101_t *radio = NULL;
      rc = nartis_cc1101_open(&radio, backend, &cfg.cc1101);
      if (rc == NARTIS_OK) rc = nartis_cc1101_init(radio);
      if (rc == NARTIS_OK) printf("CC1101 connected (spi=%s)\n", cfg.cc1101.spi_dev ? cfg.cc1101.spi_dev : "/dev/spidev0.0");
      else fprintf(stderr, "CC1101 not detected (check wiring/spi-dev)\n");
      nartis_cc1101_close(radio);
    } else {
      nartis_cmt2300a_t *radio = NULL;
      rc = nartis_cmt2300a_open(&radio, backend, &cfg.pins);
      if (rc == NARTIS_OK) rc = nartis_cmt2300a_init(radio);
      if (rc == NARTIS_OK) printf("CMT2300A product-id=0x%02x\n", nartis_cmt2300a_read_reg(radio, 0x01));
      nartis_cmt2300a_close(radio);
    }
  } else {
    nartis_meter_t *m = NULL;
    rc = nartis_open(&m, &cfg, backend);
    if (rc == NARTIS_OK) rc = nartis_init(m);
    if (rc == NARTIS_OK && strcmp(cmd, "scan") == 0) {
      int8_t scores[4] = {0};
      uint8_t best = 0;
      rc = nartis_scan_channels(m, scores, &best);
      if (rc == NARTIS_OK) printf("{\"scores\":[%d,%d,%d,%d],\"best\":%u}\n",
                                  scores[0], scores[1], scores[2], scores[3], best);
    } else if (rc == NARTIS_OK && strcmp(cmd, "pair") == 0) {
      rc = nartis_pair(m);
      if (rc == NARTIS_OK) printf("{\"paired\":true}\n");
    } else if (rc == NARTIS_OK && strcmp(cmd, "read") == 0) {
      if (attr_count == 0) {
        fprintf(stderr, "read requires at least one --obis\n");
        rc = NARTIS_ERR_ARG;
      } else {
        nartis_value_t values[32];
        rc = nartis_read(m, attrs, attr_count, values, 32);
        if (rc == NARTIS_OK) {
          printf("{\"values\":[");
          for (size_t i = 0; i < attr_count; ++i) {
            char obis[32], s[128];
            nartis_obis_format(&attrs[i].obis, obis, sizeof(obis));
            nartis_value_to_string(&values[i], s, sizeof(s));
            printf("%s{\"obis\":\"%s\",\"class\":%u,\"attr\":%u,\"valid\":%s,\"type\":%u,\"number\":%.17g,\"text\":\"%s\"}",
                   i ? "," : "", obis, attrs[i].class_id, attrs[i].attr_id,
                   values[i].valid ? "true" : "false", values[i].dtype,
                   nartis_value_as_double(&values[i]), s);
          }
          printf("]}\n");
        }
      }
    } else if (rc == NARTIS_OK) {
      fprintf(stderr, "unknown command: %s\n", cmd);
      usage(stderr);
      rc = NARTIS_ERR_ARG;
    }
    nartis_close(m);
  }
  nartis_sysfs_backend_close(sysfs);
  if (rc != NARTIS_OK) {
    fprintf(stderr, "%s failed: %s (%d)\n", cmd, nartis_strerror(rc), rc);
    return 1;
  }
  return 0;
}

