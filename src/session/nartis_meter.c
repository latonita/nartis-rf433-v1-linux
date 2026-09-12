#include "nartis/nartis.h"
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  FRAME_COUNTER_MARGIN = 16,
  MAX_RETRIES = 3,
};

struct nartis_meter {
  nartis_config_t cfg;
  nartis_backend_t backend;
  nartis_cmt2300a_t *radio;
  nartis_rf_ctx_t rf;
  nartis_session_state_t session;
  uint8_t data_seq;
  uint8_t beacon_seq;
  uint8_t active_channel;
  int8_t rssi[4];
  bool initialized;
  bool paired;
  bool session_primed;
  char meter_serial[32];
  char ciu_serial[32];
  char ciu_address_hex[24];
  char session_path[256];
};

static void sleep_ms(nartis_meter_t *m, uint32_t ms) {
  m->backend.delay_us(m->backend.ctx, ms * 1000u);
}

static uint32_t simple_hash(const char *s) {
  uint32_t h = 2166136261u;
  for (; s && *s; ++s) {
    h ^= (uint8_t)*s;
    h *= 16777619u;
  }
  return h;
}

static void set_bootstrap_key(nartis_meter_t *m) {
  static const uint8_t salt[4] = {0xBD, 0x02, 0x9B, 0xBE};
  uint8_t key[16] = {0};
  size_t n = strlen(m->meter_serial);
  if (n > 12) n = 12;
  memcpy(key, m->meter_serial, n);
  memcpy(key + 12, salt, 4);
  nartis_rf_set_aes_key(&m->rf, key);
}

static int derive_address(nartis_meter_t *m) {
  uint8_t b[8];
  if (m->ciu_address_hex[0]) {
    int rc = nartis_hex_to_bytes(m->ciu_address_hex, b, 8);
    if (rc != NARTIS_OK) return rc;
    nartis_rf_set_address(&m->rf, nartis_rf_address_from_bytes(b));
    return NARTIS_OK;
  }
  const char *serial = m->ciu_serial[0] ? m->ciu_serial : "NARTIS0000001";
  nartis_rf_set_address(&m->rf, nartis_rf_address_derive(serial, simple_hash(serial)));
  return NARTIS_OK;
}

static void sync_session_from_rf(nartis_meter_t *m) {
  memset(&m->session, 0, sizeof(m->session));
  m->session.version = 1;
  nartis_rf_get_aes_key(&m->rf, m->session.aes_key);
  nartis_rf_address_to_bytes(&m->rf.meter_address, m->session.meter_addr);
  nartis_rf_address_to_bytes(&m->rf.address, m->session.ciu_addr);
  m->session.frame_counter = m->rf.frame_counter;
  m->session.last_rx_counter = m->rf.last_rx_counter;
  m->session.last_nested_rx_counter = m->rf.last_nested_rx_counter;
  m->session.data_seq = m->data_seq;
  m->session.beacon_seq = m->beacon_seq;
  m->session.paired = m->paired ? 1 : 0;
  m->session.session_primed = m->session_primed ? 1 : 0;
}

static void sync_rf_from_session(nartis_meter_t *m) {
  nartis_rf_set_aes_key(&m->rf, m->session.aes_key);
  nartis_rf_set_meter_address(&m->rf, nartis_rf_address_from_bytes(m->session.meter_addr));
  m->rf.frame_counter = m->session.frame_counter + FRAME_COUNTER_MARGIN;
  m->rf.last_rx_counter = m->session.last_rx_counter;
  m->rf.last_nested_rx_counter = m->session.last_nested_rx_counter;
  m->data_seq = m->session.data_seq ? m->session.data_seq : 2;
  m->beacon_seq = m->session.beacon_seq ? m->session.beacon_seq : 1;
  m->paired = m->session.paired != 0;
  m->session_primed = m->session.session_primed != 0;
}

int nartis_save_session(nartis_meter_t *m) {
  if (!m || !m->session_path[0]) return NARTIS_ERR_ARG;
  sync_session_from_rf(m);
  return nartis_session_store_save(m->session_path, &m->session);
}

int nartis_load_session(nartis_meter_t *m) {
  if (!m || !m->session_path[0]) return NARTIS_ERR_ARG;
  int rc = nartis_session_store_load(m->session_path, &m->session);
  if (rc != NARTIS_OK) return rc;
  uint8_t ciu[8];
  nartis_rf_address_to_bytes(&m->rf.address, ciu);
  if (memcmp(ciu, m->session.ciu_addr, 8) != 0) return NARTIS_ERR_STATE;
  if (m->meter_serial[0] && memcmp(m->session.aes_key, m->meter_serial, 12) != 0) return NARTIS_ERR_STATE;
  sync_rf_from_session(m);
  return NARTIS_OK;
}

const nartis_session_state_t *nartis_session(const nartis_meter_t *m) {
  return m ? &m->session : NULL;
}

int nartis_open(nartis_meter_t **out, const nartis_config_t *cfg,
                const nartis_backend_t *backend) {
  if (!out || !cfg || !backend || !cfg->meter_serial) return NARTIS_ERR_ARG;
  nartis_meter_t *m = calloc(1, sizeof(*m));
  if (!m) return NARTIS_ERR_IO;
  m->cfg = *cfg;
  m->backend = *backend;
  snprintf(m->meter_serial, sizeof(m->meter_serial), "%s", cfg->meter_serial);
  if (cfg->ciu_serial) snprintf(m->ciu_serial, sizeof(m->ciu_serial), "%s", cfg->ciu_serial);
  if (cfg->ciu_address_hex) snprintf(m->ciu_address_hex, sizeof(m->ciu_address_hex), "%s", cfg->ciu_address_hex);
  if (cfg->session_path) snprintf(m->session_path, sizeof(m->session_path), "%s", cfg->session_path);
  m->cfg.fixed_channel = cfg->fixed_channel;
  if (m->cfg.fixed_channel < -1) m->cfg.fixed_channel = -1;
  if (m->cfg.batch_size == 0 || m->cfg.batch_size > NARTIS_MAX_LIST_ATTRS) m->cfg.batch_size = 5;
  if (m->cfg.rx_timeout_ms == 0) m->cfg.rx_timeout_ms = 3000;
  if (m->cfg.rx_reply_timeout_ms == 0) m->cfg.rx_reply_timeout_ms = 900;
  nartis_rf_init(&m->rf);
  derive_address(m);
  set_bootstrap_key(m);
  m->data_seq = 2;
  m->beacon_seq = 1;
  m->active_channel = 2;
  int rc = nartis_cmt2300a_open(&m->radio, backend, &cfg->pins);
  if (rc != NARTIS_OK) {
    free(m);
    return rc;
  }
  nartis_cmt2300a_use_custom_channels(m->radio, cfg->use_alternative_channels);
  *out = m;
  return NARTIS_OK;
}

void nartis_close(nartis_meter_t *m) {
  if (!m) return;
  nartis_cmt2300a_close(m->radio);
  free(m);
}

int nartis_init(nartis_meter_t *m) {
  if (!m) return NARTIS_ERR_ARG;
  int rc = nartis_cmt2300a_init(m->radio);
  if (rc != NARTIS_OK) return rc;
  nartis_cmt2300a_test_gpio3(m->radio);
  m->initialized = true;
  if (m->session_path[0]) nartis_load_session(m);
  return NARTIS_OK;
}

int nartis_scan_channels(nartis_meter_t *m, int8_t scores[4], uint8_t *best_ch) {
  if (!m) return NARTIS_ERR_ARG;
  return nartis_cmt2300a_scan_channels(m->radio, scores, best_ch);
}

static size_t build_pair_probe(nartis_meter_t *m, uint8_t *out, size_t max) {
  size_t n = strlen(m->meter_serial);
  if (max < n + 1) return 0;
  out[0] = '0';
  memcpy(out + 1, m->meter_serial, n);
  return n + 1;
}

static void build_rtc_timestamp(uint8_t out[6]) {
  time_t epoch = time(NULL);
  if (epoch < 1704067200) epoch = 1704067200;
  struct tm tmv;
  gmtime_r(&epoch, &tmv);
  uint8_t yr = (uint8_t)((tmv.tm_year + 1900) % 100);
  uint8_t mo = (uint8_t)(tmv.tm_mon + 1);
  uint8_t day = (uint8_t)tmv.tm_mday;
  uint8_t hr = (uint8_t)tmv.tm_hour;
  uint8_t mn = (uint8_t)tmv.tm_min;
  uint8_t sec = (uint8_t)tmv.tm_sec;
  uint8_t dow = (uint8_t)(tmv.tm_wday == 0 ? 7 : tmv.tm_wday);
  out[0] = sec & 0x3F;
  out[1] = mn & 0x3F;
  out[2] = (hr & 0x1F) | ((dow & 0x07) << 5);
  out[3] = (day & 0x1F) | ((yr & 0x07) << 5);
  out[4] = (mo & 0x0F) | (((yr >> 3) & 0x0F) << 4);
  out[5] = 0;
}

static size_t build_beacon(uint8_t *out, size_t max) {
  static const uint8_t head[16] = {
    0x0D,0xFD,0x0D,0x04,0x31,0x32,0x33,0x34,
    0x0D,0xFD,0x0F,0x02,0x07,0x04,0x06,0x6D
  };
  static const uint8_t tail[7] = {0x04,0xFD,0x17,0x00,0x00,0x00,0x00};
  if (max < 29) return 0;
  memcpy(out, head, sizeof(head));
  build_rtc_timestamp(out + 16);
  memcpy(out + 22, tail, sizeof(tail));
  return 29;
}

static int tx_frame(nartis_meter_t *m, nartis_rf_frame_type_t type,
                    uint8_t seq, const uint8_t *payload, size_t payload_len) {
  uint8_t frame[NARTIS_MAX_RF_FRAME_SIZE];
  size_t len = nartis_rf_build_frame(&m->rf, frame, sizeof(frame), type, seq, payload, payload_len);
  if (!len) return NARTIS_ERR_PROTOCOL;
  return nartis_cmt2300a_transmit(m->radio, frame, len);
}

static int rx_frame_raw(nartis_meter_t *m, uint8_t *frame, size_t *frame_len, uint32_t timeout_ms) {
  int rc = nartis_cmt2300a_start_rx(m->radio);
  if (rc != NARTIS_OK) return rc;
  rc = nartis_cmt2300a_poll_rx(m->radio, frame, frame_len, NARTIS_MAX_RF_FRAME_SIZE, timeout_ms);
  nartis_cmt2300a_finish_rx(m->radio);
  return rc;
}

static int rx_parse(nartis_meter_t *m, uint8_t *payload, size_t payload_max,
                    uint8_t *type, uint32_t timeout_ms) {
  uint8_t frame[NARTIS_MAX_RF_FRAME_SIZE];
  size_t frame_len = 0;
  int rc = rx_frame_raw(m, frame, &frame_len, timeout_ms);
  if (rc != NARTIS_OK) return rc;
  if (!m->rf.meter_address_set && frame_len >= 10) {
    nartis_rf_set_meter_address(&m->rf, nartis_rf_address_from_bytes(frame + 2));
  }
  return nartis_rf_parse_frame(&m->rf, frame, frame_len, payload, payload_max, type);
}

static int select_channel(nartis_meter_t *m) {
  if (m->cfg.fixed_channel >= 0) {
    m->active_channel = (uint8_t)(m->cfg.fixed_channel & 3);
  } else if (!m->cfg.use_alternative_channels) {
    uint8_t best = 1;
    nartis_cmt2300a_scan_channels(m->radio, m->rssi, &best);
    m->active_channel = best;
  } else {
    m->active_channel = 2;
  }
  nartis_cmt2300a_set_channel(m->radio, m->active_channel);
  nartis_rf_set_channel_quality(&m->rf, m->active_channel, m->rssi[m->active_channel]);
  return NARTIS_OK;
}

int nartis_pair(nartis_meter_t *m) {
  if (!m) return NARTIS_ERR_ARG;
  if (!m->initialized) {
    int rc = nartis_init(m);
    if (rc != NARTIS_OK) return rc;
  }
  set_bootstrap_key(m);
  m->data_seq = 2;
  m->beacon_seq = 1;
  m->paired = false;
  m->session_primed = false;
  m->rf.frame_counter = 0;
  m->rf.last_rx_counter = 0;
  m->rf.last_nested_rx_counter = 0;
  m->rf.meter_address_set = false;
  select_channel(m);

  uint8_t payload[256];
  uint8_t rx_type = 0;
  uint8_t probe[32];
  size_t probe_len = build_pair_probe(m, probe, sizeof(probe));
  if (!probe_len) return NARTIS_ERR_ARG;

  for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
    int rc = tx_frame(m, RF_FRAME_DATA, m->data_seq++, probe, probe_len);
    if (rc != NARTIS_OK) return rc;
    int plen = rx_parse(m, payload, sizeof(payload), &rx_type, m->cfg.rx_timeout_ms);
    if (plen >= 0 && rx_type == RX_TYPE_PRESENCE_ACK) break;
    if (attempt == MAX_RETRIES - 1) return plen < 0 ? plen : NARTIS_ERR_PROTOCOL;
    sleep_ms(m, 2300);
  }

  sleep_ms(m, 637);
  uint8_t beacon[32];
  size_t beacon_len = build_beacon(beacon, sizeof(beacon));
  int rc = tx_frame(m, RF_FRAME_ACK, m->data_seq++, beacon, beacon_len);
  if (rc != NARTIS_OK) return rc;
  int plen = rx_parse(m, payload, sizeof(payload), &rx_type, m->cfg.rx_timeout_ms);
  if (plen < 0 || rx_type != RX_TYPE_SESSION_SETUP) return plen < 0 ? plen : NARTIS_ERR_PROTOCOL;
  uint8_t key[16];
  if (nartis_rf_extract_session_key(&m->rf, payload, (size_t)plen, key)) {
    nartis_rf_set_aes_key(&m->rf, key);
  }

  sleep_ms(m, 5629);
  rc = tx_frame(m, RF_FRAME_PLAIN_DATA, m->beacon_seq++, probe, probe_len);
  if (rc != NARTIS_OK) return rc;
  plen = rx_parse(m, payload, sizeof(payload), &rx_type, m->cfg.rx_timeout_ms);
  if (plen < 0 || (rx_type != RX_TYPE_REQUEST_ACK && rx_type != RX_TYPE_SHORT_ACK)) return plen < 0 ? plen : NARTIS_ERR_PROTOCOL;
  m->paired = true;
  m->session_primed = false;
  if (m->session_path[0]) nartis_save_session(m);
  return NARTIS_OK;
}

int nartis_repair(nartis_meter_t *m) {
  if (!m) return NARTIS_ERR_ARG;
  memset(&m->session, 0, sizeof(m->session));
  m->paired = false;
  m->session_primed = false;
  return nartis_pair(m);
}

static int tx_dlms(nartis_meter_t *m, const uint8_t *apdu, size_t apdu_len) {
  uint8_t plain[128];
  if (apdu_len + 8 > sizeof(plain)) return NARTIS_ERR_ARG;
  plain[0] = 0x00; plain[1] = 0x01;
  plain[2] = 0x00; plain[3] = 0x66;
  plain[4] = 0x00; plain[5] = 0x01;
  plain[6] = (uint8_t)(apdu_len >> 8);
  plain[7] = (uint8_t)apdu_len;
  memcpy(plain + 8, apdu, apdu_len);
  return tx_frame(m, RF_FRAME_ACK, m->data_seq++, plain, apdu_len + 8);
}

static int receive_data_response(nartis_meter_t *m, nartis_value_t *values, uint8_t expected,
                                 const uint16_t *class_ids, uint8_t *got) {
  uint8_t payload[256], dlms[256], type = 0;
  int plen = rx_parse(m, payload, sizeof(payload), &type, m->cfg.rx_timeout_ms);
  if (plen < 0) return plen;
  if (type == RX_TYPE_REQUEST_ACK) {
    uint8_t beacon[32];
    size_t beacon_len = build_beacon(beacon, sizeof(beacon));
    int rc = tx_frame(m, RF_FRAME_BEACON, m->beacon_seq++, beacon, beacon_len);
    if (rc != NARTIS_OK) return rc;
    plen = rx_parse(m, payload, sizeof(payload), &type, m->cfg.rx_timeout_ms);
    if (plen < 0) return plen;
  }
  if (type != RX_TYPE_DATA) return NARTIS_ERR_PROTOCOL;
  int dlen = nartis_rf_parse_nested(&m->rf, payload, (size_t)plen, dlms, sizeof(dlms), true);
  if (dlen < 0) return dlen;
  if (!nartis_dlms_parse_read_response_list(dlms, (size_t)dlen, values, expected, got, class_ids)) return NARTIS_ERR_PROTOCOL;
  uint8_t beacon[32];
  size_t beacon_len = build_beacon(beacon, sizeof(beacon));
  int rc = tx_frame(m, RF_FRAME_BEACON, m->beacon_seq++, beacon, beacon_len);
  if (rc == NARTIS_OK) {
    uint8_t ack_type = 0;
    int ack_len = rx_parse(m, payload, sizeof(payload), &ack_type, m->cfg.rx_reply_timeout_ms);
    (void)ack_len;
  }
  return NARTIS_OK;
}

int nartis_read(nartis_meter_t *m, const nartis_attr_t *attrs, size_t attr_count,
                nartis_value_t *values, size_t value_capacity) {
  if (!m || !attrs || !values || attr_count > value_capacity) return NARTIS_ERR_ARG;
  if (!m->initialized) {
    int rc = nartis_init(m);
    if (rc != NARTIS_OK) return rc;
  }
  if (!m->paired) {
    int rc = nartis_pair(m);
    if (rc != NARTIS_OK) return rc;
  }
  select_channel(m);
  memset(values, 0, sizeof(values[0]) * value_capacity);

  uint8_t beacon[32];
  size_t beacon_len = build_beacon(beacon, sizeof(beacon));
  int rc = tx_frame(m, RF_FRAME_BEACON, m->beacon_seq++, beacon, beacon_len);
  if (rc != NARTIS_OK) return rc;
  uint8_t payload[256], type = 0;
  int plen = rx_parse(m, payload, sizeof(payload), &type, m->cfg.rx_timeout_ms);
  if (plen < 0 || type != RX_TYPE_SHORT_ACK) return plen < 0 ? plen : NARTIS_ERR_PROTOCOL;

  if (!m->session_primed) {
    static const nartis_obis_t vendor = {{0x00,0x00,0x60,0x80,0x03,0xFF}};
    uint8_t apdu[32];
    size_t apdu_len = nartis_dlms_build_get_request_normal(apdu, sizeof(apdu), &vendor, 1, 2);
    rc = tx_dlms(m, apdu, apdu_len);
    if (rc != NARTIS_OK) return rc;
    nartis_value_t dummy[NARTIS_MAX_LIST_ATTRS];
    uint8_t got = 0;
    rc = receive_data_response(m, dummy, NARTIS_MAX_LIST_ATTRS, NULL, &got);
    if (rc != NARTIS_OK) return rc;
    m->session_primed = true;
    if (m->session_path[0]) nartis_save_session(m);
  }

  size_t idx = 0;
  while (idx < attr_count) {
    uint8_t batch = (uint8_t)(attr_count - idx);
    if (batch > m->cfg.batch_size) batch = m->cfg.batch_size;
    uint8_t apdu[4 + 10 * NARTIS_MAX_LIST_ATTRS];
    size_t apdu_len = nartis_dlms_build_get_request_with_list(apdu, sizeof(apdu), attrs + idx, batch);
    if (!apdu_len) return NARTIS_ERR_ARG;
    rc = tx_dlms(m, apdu, apdu_len);
    if (rc != NARTIS_OK) return rc;
    uint16_t class_ids[NARTIS_MAX_LIST_ATTRS] = {0};
    for (uint8_t i = 0; i < batch; ++i) class_ids[i] = attrs[idx + i].class_id;
    uint8_t got = 0;
    rc = receive_data_response(m, values + idx, batch, class_ids, &got);
    if (rc != NARTIS_OK) return rc;
    if (got != batch) return NARTIS_ERR_PROTOCOL;
    idx += batch;
  }
  if (m->session_path[0]) nartis_save_session(m);
  nartis_cmt2300a_go_sleep(m->radio);
  return NARTIS_OK;
}

