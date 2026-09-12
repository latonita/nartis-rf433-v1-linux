#ifndef NARTIS_INTERNAL_H
#define NARTIS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nartis/nartis_types.h"

#define NARTIS_RF_MANUFACTURER_ID 0x2CCD
#define NARTIS_RF_VERSION 0x50
#define NARTIS_RF_DEVICE_TYPE_CIU 0x25
#define NARTIS_RF_DEVICE_TYPE_METER 0x02

typedef enum {
  RF_FRAME_BEACON = 0x08,
  RF_FRAME_DATA = 0x46,
  RF_FRAME_ACK = 0x44,
  RF_FRAME_PLAIN_DATA = 0x00,
} nartis_rf_frame_type_t;

enum {
  RX_TYPE_PRESENCE_ACK = 0x06,
  RX_TYPE_SHORT_ACK = 0x40,
  RX_TYPE_DATA = 0x43,
  RX_TYPE_SESSION_SETUP = 0x53,
  RX_TYPE_REQUEST_ACK = 0x5B,
};

typedef struct {
  nartis_rf_address_t address;
  nartis_rf_address_t meter_address;
  bool meter_address_set;
  uint8_t aes_key[NARTIS_AES_KEY_SIZE];
  uint32_t frame_counter;
  uint32_t last_rx_counter;
  uint32_t last_nested_rx_counter;
  uint8_t last_tx_sequence;
  bool has_sent_first_tx;
  uint8_t channel;
  int8_t rssi_dbm;
  uint8_t work[NARTIS_MAX_RF_FRAME_SIZE];
} nartis_rf_ctx_t;

void nartis_rf_address_to_bytes(const nartis_rf_address_t *addr, uint8_t out[8]);
nartis_rf_address_t nartis_rf_address_from_bytes(const uint8_t in[8]);
nartis_rf_address_t nartis_rf_address_derive(const char *ciu_serial, uint32_t seed);
int nartis_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len);

uint16_t nartis_crc16_dnp(const uint8_t *data, size_t len);
bool nartis_crc16_verify(const uint8_t *data, size_t len);
void nartis_rf_init(nartis_rf_ctx_t *ctx);
void nartis_rf_set_address(nartis_rf_ctx_t *ctx, nartis_rf_address_t addr);
void nartis_rf_set_meter_address(nartis_rf_ctx_t *ctx, nartis_rf_address_t addr);
void nartis_rf_set_aes_key(nartis_rf_ctx_t *ctx, const uint8_t key[16]);
void nartis_rf_get_aes_key(const nartis_rf_ctx_t *ctx, uint8_t key[16]);
void nartis_rf_set_channel_quality(nartis_rf_ctx_t *ctx, uint8_t ch, int8_t rssi);
size_t nartis_rf_build_frame(nartis_rf_ctx_t *ctx, uint8_t *out, size_t out_max,
                             nartis_rf_frame_type_t type, uint8_t sequence,
                             const uint8_t *payload, size_t payload_len);
int nartis_rf_parse_frame(nartis_rf_ctx_t *ctx, const uint8_t *in, size_t in_len,
                          uint8_t *payload_out, size_t payload_max, uint8_t *type_out);
int nartis_rf_parse_nested(nartis_rf_ctx_t *ctx, const uint8_t *payload, size_t payload_len,
                           uint8_t *plain_out, size_t plain_max, bool mutate_counter);
bool nartis_rf_extract_session_key(const nartis_rf_ctx_t *ctx, const uint8_t *payload,
                                   size_t len, uint8_t out_key[16]);
int nartis_rf_encrypt(nartis_rf_ctx_t *ctx, uint8_t *data, size_t len, size_t buf_max,
                      uint32_t counter);
int nartis_rf_decrypt(nartis_rf_ctx_t *ctx, uint8_t *data, size_t len_with_tag,
                      uint32_t counter);
int nartis_rf_decrypt_with_key(const nartis_rf_ctx_t *ctx, const uint8_t key[16],
                               uint8_t *data, size_t len_with_tag, uint32_t counter);

size_t nartis_dlms_build_get_request_with_list(uint8_t *out, size_t max,
                                               const nartis_attr_t *attrs, uint8_t count);
size_t nartis_dlms_build_get_request_normal(uint8_t *out, size_t max,
                                            const nartis_obis_t *obis,
                                            uint16_t class_id, uint8_t attr_id);
bool nartis_dlms_parse_read_response_list(const uint8_t *data, size_t len,
                                          nartis_value_t *values, uint8_t max_results,
                                          uint8_t *count_out,
                                          const uint16_t *class_ids);

int nartis_session_store_load(const char *path, nartis_session_state_t *state);
int nartis_session_store_save(const char *path, const nartis_session_state_t *state);

#endif
