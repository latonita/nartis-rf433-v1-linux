#include "nartis/nartis.h"
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  FRAME_COUNTER_MARGIN = 16,
  MAX_RETRIES = 3,
  MAX_LEARNED_TYPES = 32,
  MAX_BATCH_TYPE_RETRIES = 2,
};

/* Per-OBIS learned DLMS type/length, used to reject a value whose type/len does
 * not match what this code returned before - a stale or misattributed reply. A
 * new type seen twice in a row is treated as a real change and accepted. */
typedef struct {
  nartis_obis_t obis;
  uint8_t learned_dtype;  /* 0xFF = not learned yet */
  uint8_t learned_len;
  uint8_t pending_dtype;  /* 0xFF = none */
  uint8_t pending_len;
  bool used;
} learned_type_t;

struct nartis_meter {
  nartis_config_t cfg;
  nartis_backend_t backend;
  nartis_radio_t radio;
  nartis_rf_ctx_t rf;
  nartis_session_state_t session;
  uint8_t data_seq;
  uint8_t beacon_seq;
  uint8_t active_channel;
  uint8_t next_get_invoke_id;  /* 4-bit counter, advances per logical GET */
  uint8_t get_invoke_id;       /* invoke-id of the GET currently in flight */
  int8_t rssi[4];
  bool initialized;
  bool paired;
  bool session_primed;
  char meter_serial[32];
  char ciu_serial[32];
  char ciu_address_hex[24];
  char session_path[256];
  learned_type_t learned[MAX_LEARNED_TYPES];
};

static bool dlms_type_is_var_len(uint8_t dtype) {
  return dtype == 0x09 || dtype == 0x0A || dtype == 0x0C ||
         dtype == 0x19 || dtype == 0x01 || dtype == 0x02;
}

static learned_type_t *learned_for(nartis_meter_t *m, const nartis_obis_t *obis) {
  int free_slot = -1;
  for (int i = 0; i < MAX_LEARNED_TYPES; ++i) {
    if (m->learned[i].used) {
      if (memcmp(m->learned[i].obis.bytes, obis->bytes, 6) == 0) return &m->learned[i];
    } else if (free_slot < 0) {
      free_slot = i;
    }
  }
  if (free_slot < 0) return NULL;
  learned_type_t *e = &m->learned[free_slot];
  e->used = true;
  e->obis = *obis;
  e->learned_dtype = 0xFF;
  e->pending_dtype = 0xFF;
  return e;
}

/* Returns true if the value may be stored, false if it should be rejected and
 * the batch re-sent (type/len differs from what this OBIS returned before). */
static bool learned_type_accept(learned_type_t *e, const nartis_value_t *v) {
  if (!e || !v->valid || e->learned_dtype == 0xFF) return true;
  bool var = dlms_type_is_var_len(e->learned_dtype);
  if (v->dtype == e->learned_dtype && (var || v->raw_len == e->learned_len)) {
    e->pending_dtype = 0xFF;
    return true;
  }
  if (e->pending_dtype == v->dtype && e->pending_len == v->raw_len) {
    e->learned_dtype = 0xFF;  /* a new type seen twice = real change */
    e->pending_dtype = 0xFF;
    return true;
  }
  e->pending_dtype = v->dtype;
  e->pending_len = v->raw_len;
  return false;
}

static void learned_type_store(learned_type_t *e, const nartis_value_t *v) {
  if (e && v->valid && e->learned_dtype == 0xFF) {
    e->learned_dtype = v->dtype;
    e->learned_len = v->raw_len;
  }
}

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

/* A CIU serial that is unique to this host but stable across runs, so a device
 * invents its own CIU address for fresh pairing without the user supplying one,
 * and keeps the same one on later runs. Derived from /etc/machine-id; falls back
 * to a fixed value if that is unavailable. */
static void default_ciu_serial(char *out, size_t n) {
  char mid[64] = {0};
  FILE *f = fopen("/etc/machine-id", "r");
  if (f) {
    if (!fgets(mid, sizeof(mid), f)) mid[0] = 0;
    fclose(f);
  }
  if (mid[0]) {
    snprintf(out, n, "%010u", (unsigned) (simple_hash(mid) % 1000000000u));
  } else {
    snprintf(out, n, "NARTIS0000001");
  }
}

static int derive_address(nartis_meter_t *m) {
  uint8_t b[8];
  if (m->ciu_address_hex[0]) {
    int rc = nartis_hex_to_bytes(m->ciu_address_hex, b, 8);
    if (rc != NARTIS_OK) return rc;
    nartis_rf_set_address(&m->rf, nartis_rf_address_from_bytes(b));
    return NARTIS_OK;
  }
  char serbuf[32];
  const char *serial;
  if (m->ciu_serial[0]) {
    serial = m->ciu_serial;
  } else {
    default_ciu_serial(serbuf, sizeof(serbuf));
    serial = serbuf;
  }
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
  if (m->ciu_address_hex[0]) {
    /* An explicit CIU was given: the session must belong to it. */
    uint8_t ciu[8];
    nartis_rf_address_to_bytes(&m->rf.address, ciu);
    if (memcmp(ciu, m->session.ciu_addr, 8) != 0) return NARTIS_ERR_STATE;
  } else {
    /* No CIU given: adopt the one the session was paired with, so a transplanted
     * session needs no --ciu-address. */
    nartis_rf_set_address(&m->rf, nartis_rf_address_from_bytes(m->session.ciu_addr));
  }
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
  m->next_get_invoke_id = 1;
  int rc;
  if (m->cfg.radio_type == NARTIS_RADIO_CC1101) {
    rc = nartis_radio_open_cc1101(&m->radio, backend, &cfg->cc1101);
  } else {
    rc = nartis_radio_open_cmt2300a(&m->radio, backend, &cfg->pins);
  }
  if (rc != NARTIS_OK) {
    free(m);
    return rc;
  }
  m->radio.ops->use_custom_channels(m->radio.dev, cfg->use_alternative_channels);
  *out = m;
  return NARTIS_OK;
}

void nartis_close(nartis_meter_t *m) {
  if (!m) return;
  if (m->radio.ops) m->radio.ops->close(m->radio.dev);
  free(m);
}

int nartis_init(nartis_meter_t *m) {
  if (!m) return NARTIS_ERR_ARG;
  int rc = m->radio.ops->init(m->radio.dev);
  if (rc != NARTIS_OK) return rc;
  m->radio.ops->test_gpio(m->radio.dev);
  m->initialized = true;
  if (m->session_path[0]) nartis_load_session(m);
  return NARTIS_OK;
}

int nartis_scan_channels(nartis_meter_t *m, int8_t scores[4], uint8_t *best_ch) {
  if (!m) return NARTIS_ERR_ARG;
  return m->radio.ops->scan_channels(m->radio.dev, scores, best_ch);
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
  return m->radio.ops->transmit(m->radio.dev, frame, len);
}

static int rx_frame_raw(nartis_meter_t *m, uint8_t *frame, size_t *frame_len, uint32_t timeout_ms) {
  int rc = m->radio.ops->start_rx(m->radio.dev);
  if (rc != NARTIS_OK) return rc;
  rc = m->radio.ops->poll_rx(m->radio.dev, frame, frame_len, NARTIS_MAX_RF_FRAME_SIZE, timeout_ms);
  m->radio.ops->finish_rx(m->radio.dev);
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
    m->radio.ops->scan_channels(m->radio.dev, m->rssi, &best);
    m->active_channel = best;
  } else {
    m->active_channel = 2;
  }
  m->radio.ops->set_channel(m->radio.dev, m->active_channel);
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

/* A SESSION_SETUP (0x53) seen mid-session means the meter re-keyed. Install the
 * new data key and persist it so the rest of the cycle uses it. Best effort: if
 * the frame yields no key, the current key is left in place. */
static bool handle_rekey_(nartis_meter_t *m, const uint8_t *payload, int plen, uint8_t type) {
  if (type != RX_TYPE_SESSION_SETUP || plen < 0) return false;
  uint8_t key[16];
  if (nartis_rf_extract_session_key(&m->rf, payload, (size_t) plen, key)) {
    nartis_rf_set_aes_key(&m->rf, key);
    if (m->session_path[0]) nartis_save_session(m);
  }
  return true;
}

/* Pacing to match the meter's get-request handshake (values from the ESPHome
 * component): after the meter acks a request it needs a gate before the data is
 * ready, polled for with beacons; a request that draws no reply is re-sent. */
enum {
  GET_REPLY_MAX_RETRIES = 6,
  READ_POLL_BEACON_DELAY_MS = 5000,
  PRIMING_POLL_BEACON_DELAY_MS = 1000,
  READ_MAX_POLL = 3,
  PRIMING_MAX_POLL = 2,
};

static int do_get_cycle(nartis_meter_t *m, const uint8_t *apdu, size_t apdu_len,
                        nartis_value_t *values, uint8_t expected,
                        const uint16_t *class_ids, uint8_t *got, bool priming,
                        int expected_invoke_id) {
  const uint32_t poll_delay = priming ? PRIMING_POLL_BEACON_DELAY_MS : READ_POLL_BEACON_DELAY_MS;
  const int max_poll = priming ? PRIMING_MAX_POLL : READ_MAX_POLL;
  uint8_t payload[256], dlms[256], type;
  int plen = -1;
  bool have_data = false;

  for (int req_try = 0; req_try < GET_REPLY_MAX_RETRIES && !have_data; ++req_try) {
    int rc = tx_dlms(m, apdu, apdu_len);
    if (rc != NARTIS_OK) return rc;
    type = 0;
    plen = rx_parse(m, payload, sizeof(payload), &type, m->cfg.rx_timeout_ms);
    if (plen >= 0 && type == RX_TYPE_SESSION_SETUP) { handle_rekey_(m, payload, plen, type); continue; }
    if (plen >= 0 && type == RX_TYPE_DATA) { have_data = true; break; }
    if (plen >= 0 && (type == RX_TYPE_REQUEST_ACK || type == RX_TYPE_SHORT_ACK)) {
      /* request accepted; poll for the data with beacons after a settle gate */
      for (int p = 0; p < max_poll && !have_data; ++p) {
        sleep_ms(m, poll_delay);
        uint8_t beacon[32];
        size_t bl = build_beacon(beacon, sizeof(beacon));
        rc = tx_frame(m, RF_FRAME_BEACON, m->beacon_seq++, beacon, bl);
        if (rc != NARTIS_OK) return rc;
        type = 0;
        plen = rx_parse(m, payload, sizeof(payload), &type, m->cfg.rx_timeout_ms);
        if (plen >= 0 && type == RX_TYPE_SESSION_SETUP) { handle_rekey_(m, payload, plen, type); break; }
        if (plen >= 0 && type == RX_TYPE_DATA) have_data = true;
      }
    }
    /* otherwise (no reply, 0x40 not-ready, or a re-key): re-send the get-request */
  }
  if (!have_data) return NARTIS_ERR_TIMEOUT;

  int dlen = nartis_rf_parse_nested(&m->rf, payload, (size_t)plen, dlms, sizeof(dlms), true);
  if (dlen < 0) return dlen;
  /* The priming request is a get-request-normal whose response is a throwaway
   * used only to engage the session - it is not a with-list response, so do not
   * parse it as one. Real batches use get-request-with-list. */
  if (!priming) {
    int resp_invoke = -1;
    bool ok = nartis_dlms_parse_read_response_list(
        dlms, (size_t)dlen, values, expected, got, class_ids,
        m->cfg.invoke_id_enforce ? expected_invoke_id : -1, m->cfg.invoke_id_enforce, &resp_invoke);
    /* Log the echo every time so the meter's invoke-id behaviour is observable
     * even when enforcement is off. */
    nartis_backend_log(&m->backend, NARTIS_LOG_INFO, "GET invoke: sent=%d recv=%d match=%s",
                       expected_invoke_id, resp_invoke,
                       (resp_invoke == (expected_invoke_id & 0x0F)) ? "yes" : "NO");
    if (!ok) return NARTIS_ERR_PROTOCOL;
  }

  /* closing beacon; the short ack that follows is not needed */
  uint8_t beacon[32];
  size_t bl = build_beacon(beacon, sizeof(beacon));
  if (tx_frame(m, RF_FRAME_BEACON, m->beacon_seq++, beacon, bl) == NARTIS_OK) {
    uint8_t at = 0;
    (void) rx_parse(m, payload, sizeof(payload), &at, m->cfg.rx_reply_timeout_ms);
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

  /* Wake the meter, retrying a few times: a single frame lost to RF noise should
   * not fail the whole read. The meter answers with a short ack (0x40) or, when
   * already mid-session, a request ack (0x5B); a 0x53 means it re-keyed, which we
   * install and then proceed. Anything else (noise/timeout) is retried. */
  uint8_t payload[256], type = 0;
  int plen = -1;
  int rc = NARTIS_OK;
  bool woke = false;
  for (int wake_try = 0; wake_try < 3 && !woke; ++wake_try) {
    uint8_t beacon[32];
    size_t beacon_len = build_beacon(beacon, sizeof(beacon));
    rc = tx_frame(m, RF_FRAME_BEACON, m->beacon_seq++, beacon, beacon_len);
    if (rc != NARTIS_OK) return rc;
    type = 0;
    plen = rx_parse(m, payload, sizeof(payload), &type, m->cfg.rx_timeout_ms);
    if (plen >= 0 && (type == RX_TYPE_SHORT_ACK || type == RX_TYPE_REQUEST_ACK)) {
      woke = true;
    } else if (plen >= 0 && type == RX_TYPE_SESSION_SETUP) {
      handle_rekey_(m, payload, plen, type);
      woke = true;
    }
  }
  if (!woke) return NARTIS_ERR_TIMEOUT;

  if (!m->session_primed) {
    static const nartis_obis_t vendor = {{0x00,0x00,0x60,0x80,0x03,0xFF}};
    m->get_invoke_id = m->next_get_invoke_id;
    m->next_get_invoke_id = (uint8_t)((m->next_get_invoke_id + 1) & 0x0F);
    uint8_t apdu[32];
    size_t apdu_len = nartis_dlms_build_get_request_normal(apdu, sizeof(apdu), &vendor, 1, 2,
                                                           m->get_invoke_id);
    nartis_value_t dummy[NARTIS_MAX_LIST_ATTRS];
    uint8_t got = 0;
    rc = do_get_cycle(m, apdu, apdu_len, dummy, NARTIS_MAX_LIST_ATTRS, NULL, &got, true,
                      m->get_invoke_id);
    if (rc != NARTIS_OK) return rc;
    m->session_primed = true;
    if (m->session_path[0]) nartis_save_session(m);
  }

  size_t idx = 0;
  while (idx < attr_count) {
    uint8_t batch = (uint8_t)(attr_count - idx);
    if (batch > m->cfg.batch_size) batch = m->cfg.batch_size;
    m->get_invoke_id = m->next_get_invoke_id;
    m->next_get_invoke_id = (uint8_t)((m->next_get_invoke_id + 1) & 0x0F);
    uint8_t apdu[4 + 10 * NARTIS_MAX_LIST_ATTRS];
    size_t apdu_len = nartis_dlms_build_get_request_with_list(apdu, sizeof(apdu), attrs + idx, batch,
                                                              m->get_invoke_id);
    if (!apdu_len) return NARTIS_ERR_ARG;
    uint16_t class_ids[NARTIS_MAX_LIST_ATTRS] = {0};
    for (uint8_t i = 0; i < batch; ++i) class_ids[i] = attrs[idx + i].class_id;

    int type_retries = 0;
    for (;;) {
      uint8_t got = 0;
      rc = do_get_cycle(m, apdu, apdu_len, values + idx, batch, class_ids, &got, false,
                        m->get_invoke_id);
      if (rc != NARTIS_OK) return rc;
      if (got != batch) return NARTIS_ERR_PROTOCOL;

      /* PR7 backstop: reject a batch whose value types/lengths differ from what
       * these OBIS codes returned before (a stale/misattributed reply) and
       * re-send; a new type seen twice is accepted as a genuine change. */
      bool all_ok = true;
      for (uint8_t i = 0; i < batch; ++i) {
        learned_type_t *e = learned_for(m, &attrs[idx + i].obis);
        if (!learned_type_accept(e, &values[idx + i])) {
          all_ok = false;
          nartis_backend_log(&m->backend, NARTIS_LOG_WARN,
                             "value type/len mismatch for attr %zu - re-sending batch",
                             idx + (size_t) i);
        }
      }
      if (all_ok || ++type_retries > MAX_BATCH_TYPE_RETRIES) {
        for (uint8_t i = 0; i < batch; ++i)
          learned_type_store(learned_for(m, &attrs[idx + i].obis), &values[idx + i]);
        break;
      }
    }
    idx += batch;
  }
  if (m->session_path[0]) nartis_save_session(m);
  m->radio.ops->go_sleep(m->radio.dev);
  return NARTIS_OK;
}

