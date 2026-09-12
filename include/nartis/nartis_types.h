#ifndef NARTIS_TYPES_H
#define NARTIS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NARTIS_AES_KEY_SIZE 16
#define NARTIS_AES_NONCE_SIZE 12
#define NARTIS_AES_TAG_SIZE 12
#define NARTIS_MAX_RF_FRAME_SIZE 512
#define NARTIS_MAX_DLMS_APDU_SIZE 400
#define NARTIS_MAX_LIST_ATTRS 10

typedef enum {
  NARTIS_OK = 0,
  NARTIS_ERR_ARG = -1,
  NARTIS_ERR_IO = -2,
  NARTIS_ERR_TIMEOUT = -3,
  NARTIS_ERR_PROTOCOL = -4,
  NARTIS_ERR_CRC = -5,
  NARTIS_ERR_CRYPTO = -6,
  NARTIS_ERR_REPLAY = -7,
  NARTIS_ERR_UNSUPPORTED = -8,
  NARTIS_ERR_STATE = -9,
} nartis_status_t;

typedef enum {
  NARTIS_LOG_ERROR = 0,
  NARTIS_LOG_WARN = 1,
  NARTIS_LOG_INFO = 2,
  NARTIS_LOG_DEBUG = 3,
  NARTIS_LOG_TRACE = 4,
} nartis_log_level_t;

typedef struct {
  uint8_t bytes[6];
} nartis_obis_t;

typedef struct {
  uint16_t class_id;
  nartis_obis_t obis;
  uint8_t attr_id;
} nartis_attr_t;

typedef struct {
  uint8_t dtype;
  uint8_t raw_len;
  uint8_t raw[64];
  bool valid;
} nartis_value_t;

typedef struct {
  uint16_t manufacturer_id;
  uint32_t address_id;
  uint8_t version;
  uint8_t device_type;
} nartis_rf_address_t;

typedef struct {
  uint32_t version;
  uint8_t aes_key[NARTIS_AES_KEY_SIZE];
  uint8_t meter_addr[8];
  uint8_t ciu_addr[8];
  uint32_t frame_counter;
  uint32_t last_rx_counter;
  uint32_t last_nested_rx_counter;
  uint8_t data_seq;
  uint8_t beacon_seq;
  uint8_t paired;
  uint8_t session_primed;
} nartis_session_state_t;

typedef enum {
  NARTIS_DLMS_NULL_DATA = 0x00,
  NARTIS_DLMS_ARRAY = 0x01,
  NARTIS_DLMS_STRUCTURE = 0x02,
  NARTIS_DLMS_BOOLEAN = 0x03,
  NARTIS_DLMS_BIT_STRING = 0x04,
  NARTIS_DLMS_INT32 = 0x05,
  NARTIS_DLMS_UINT32 = 0x06,
  NARTIS_DLMS_OCTET_STRING = 0x09,
  NARTIS_DLMS_VISIBLE_STRING = 0x0A,
  NARTIS_DLMS_UTF8_STRING = 0x0C,
  NARTIS_DLMS_BCD = 0x0D,
  NARTIS_DLMS_INT8 = 0x0F,
  NARTIS_DLMS_INT16 = 0x10,
  NARTIS_DLMS_UINT8 = 0x11,
  NARTIS_DLMS_UINT16 = 0x12,
  NARTIS_DLMS_INT64 = 0x14,
  NARTIS_DLMS_UINT64 = 0x15,
  NARTIS_DLMS_ENUM = 0x16,
  NARTIS_DLMS_FLOAT32 = 0x17,
  NARTIS_DLMS_FLOAT64 = 0x18,
  NARTIS_DLMS_DATE_TIME = 0x19,
  NARTIS_DLMS_DATE = 0x1A,
  NARTIS_DLMS_TIME = 0x1B,
} nartis_dlms_type_t;

const char *nartis_strerror(int status);
int nartis_obis_parse(const char *text, nartis_obis_t *out);
void nartis_obis_format(const nartis_obis_t *obis, char *out, size_t out_size);
double nartis_value_as_double(const nartis_value_t *value);
void nartis_value_to_string(const nartis_value_t *value, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif

