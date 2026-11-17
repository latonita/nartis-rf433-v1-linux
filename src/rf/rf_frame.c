#include "internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CI_FIELD_NORMAL = 0x7A,
  CI_FIELD_SPECIAL = 0x8A,
  AES_GCM_FLAG = 0x29,
  RF_TX_LENGTH = 0,
  RF_TX_FLAGS = 1,
  RF_TX_ADDR = 2,
  RF_TX_CI_FIELD = 10,
  RF_TX_SEQUENCE = 11,
  RF_TX_CHANNEL_BYTE = 12,
  RF_TX_ENC_FLAG = 13,
  RF_TX_SEC_FLAG = 14,
  RF_TX_HDR_SIZE = 15,
  RF_TX_ENC_DATA_LEN = 15,
  RF_TX_ENC_ZERO = 16,
  RF_TX_ENC_COUNTER = 17,
  RF_TX_ENC_CIPHER = 21,
  RF_RX_LENGTH = 0,
  RF_RX_TYPE = 1,
  RF_RX_ADDR = 2,
  RF_RX_PAYLOAD = 13,
  RF_RX_HDR_SIZE = 13,
  CRC_DUAL_THRESHOLD = 128,
  CRC1_OFFSET = 0x7E,
};

static const uint16_t crc_table[256] = {
  0x0000,0x3D65,0x7ACA,0x47AF,0xF594,0xC8F1,0x8F5E,0xB23B,0xD64D,0xEB28,0xAC87,0x91E2,0x23D9,0x1EBC,0x5913,0x6476,
  0x91FF,0xAC9A,0xEB35,0xD650,0x646B,0x590E,0x1EA1,0x23C4,0x47B2,0x7AD7,0x3D78,0x001D,0xB226,0x8F43,0xC8EC,0xF589,
  0x1E9B,0x23FE,0x6451,0x5934,0xEB0F,0xD66A,0x91C5,0xACA0,0xC8D6,0xF5B3,0xB21C,0x8F79,0x3D42,0x0027,0x4788,0x7AED,
  0x8F64,0xB201,0xF5AE,0xC8CB,0x7AF0,0x4795,0x003A,0x3D5F,0x5929,0x644C,0x23E3,0x1E86,0xACBD,0x91D8,0xD677,0xEB12,
  0x3D36,0x0053,0x47FC,0x7A99,0xC8A2,0xF5C7,0xB268,0x8F0D,0xEB7B,0xD61E,0x91B1,0xACD4,0x1EEF,0x238A,0x6425,0x5940,
  0xACC9,0x91AC,0xD603,0xEB66,0x595D,0x6438,0x2397,0x1EF2,0x7A84,0x47E1,0x004E,0x3D2B,0x8F10,0xB275,0xF5DA,0xC8BF,
  0x23AD,0x1EC8,0x5967,0x6402,0xD639,0xEB5C,0xACF3,0x9196,0xF5E0,0xC885,0x8F2A,0xB24F,0x0074,0x3D11,0x7ABE,0x47DB,
  0xB252,0x8F37,0xC898,0xF5FD,0x47C6,0x7AA3,0x3D0C,0x0069,0x641F,0x597A,0x1ED5,0x23B0,0x918B,0xACEE,0xEB41,0xD624,
  0x7A6C,0x4709,0x00A6,0x3DC3,0x8FF8,0xB29D,0xF532,0xC857,0xAC21,0x9144,0xD6EB,0xEB8E,0x59B5,0x64D0,0x237F,0x1E1A,
  0xEB93,0xD6F6,0x9159,0xAC3C,0x1E07,0x2362,0x64CD,0x59A8,0x3DDE,0x00BB,0x4714,0x7A71,0xC84A,0xF52F,0xB280,0x8FE5,
  0x64F7,0x5992,0x1E3D,0x2358,0x9163,0xAC06,0xEBA9,0xD6CC,0xB2BA,0x8FDF,0xC870,0xF515,0x472E,0x7A4B,0x3DE4,0x0081,
  0xF508,0xC86D,0x8FC2,0xB2A7,0x009C,0x3DF9,0x7A56,0x4733,0x2345,0x1E20,0x598F,0x64EA,0xD6D1,0xEBB4,0xAC1B,0x917E,
  0x475A,0x7A3F,0x3D90,0x00F5,0xB2CE,0x8FAB,0xC804,0xF561,0x9117,0xAC72,0xEBDD,0xD6B8,0x6483,0x59E6,0x1E49,0x232C,
  0xD6A5,0xEBC0,0xAC6F,0x910A,0x2331,0x1E54,0x59FB,0x649E,0x00E8,0x3D8D,0x7A22,0x4747,0xF57C,0xC819,0x8FB6,0xB2D3,
  0x59C1,0x64A4,0x230B,0x1E6E,0xAC55,0x9130,0xD69F,0xEBFA,0x8F8C,0xB2E9,0xF546,0xC823,0x7A18,0x477D,0x00D2,0x3DB7,
  0xC83E,0xF55B,0xB2F4,0x8F91,0x3DAA,0x00CF,0x4760,0x7A05,0x1E73,0x2316,0x64B9,0x59DC,0xEBE7,0xD682,0x912D,0xAC48,
};

uint16_t nartis_crc16_dnp(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; ++i) crc = (uint16_t)((crc << 8) ^ crc_table[((crc >> 8) ^ data[i]) & 0xFF]);
  return (uint16_t)~crc;
}

bool nartis_crc16_verify(const uint8_t *data, size_t len) {
  if (len < 3) return false;
  uint16_t computed = nartis_crc16_dnp(data, len - 2);
  uint16_t stored = ((uint16_t)data[len - 2] << 8) | data[len - 1];
  return computed == stored;
}

static size_t crc_insert(uint8_t *buf, size_t body_size, size_t out_max) {
  if (body_size <= 126) {
    if (body_size + 2 > out_max) return 0;
    buf[0] = (uint8_t)(body_size + 1);
    uint16_t crc = nartis_crc16_dnp(buf, body_size);
    buf[body_size] = (uint8_t)(crc >> 8);
    buf[body_size + 1] = (uint8_t)crc;
    return body_size + 2;
  }
  if (body_size + 4 > out_max) return 0;
  buf[0] = (uint8_t)(body_size + 3);
  size_t tail_size = body_size - CRC1_OFFSET;
  memmove(buf + CRC1_OFFSET + 2, buf + CRC1_OFFSET, tail_size);
  uint16_t crc1 = nartis_crc16_dnp(buf, CRC1_OFFSET);
  buf[CRC1_OFFSET] = (uint8_t)(crc1 >> 8);
  buf[CRC1_OFFSET + 1] = (uint8_t)crc1;
  uint16_t crc2 = nartis_crc16_dnp(buf + 0x80, tail_size);
  buf[0x80 + tail_size] = (uint8_t)(crc2 >> 8);
  buf[0x80 + tail_size + 1] = (uint8_t)crc2;
  return body_size + 4;
}

static int crc_strip(uint8_t *buf, size_t frame_size) {
  if (frame_size < 3) return -1;
  if (frame_size <= CRC_DUAL_THRESHOLD) return nartis_crc16_verify(buf, frame_size) ? (int)(frame_size - 2) : -1;
  if (frame_size < 0x82) return -1;
  uint16_t crc1_obs = ((uint16_t)buf[CRC1_OFFSET] << 8) | buf[CRC1_OFFSET + 1];
  if (crc1_obs != nartis_crc16_dnp(buf, CRC1_OFFSET)) return -1;
  size_t tail_size = frame_size - 0x80 - 2;
  uint16_t crc2_obs = ((uint16_t)buf[frame_size - 2] << 8) | buf[frame_size - 1];
  if (crc2_obs != nartis_crc16_dnp(buf + 0x80, tail_size)) return -1;
  memmove(buf + CRC1_OFFSET, buf + 0x80, tail_size);
  return (int)(CRC1_OFFSET + tail_size);
}

void nartis_rf_address_to_bytes(const nartis_rf_address_t *addr, uint8_t out[8]) {
  out[0] = (uint8_t)addr->manufacturer_id;
  out[1] = (uint8_t)(addr->manufacturer_id >> 8);
  out[2] = (uint8_t)addr->address_id;
  out[3] = (uint8_t)(addr->address_id >> 8);
  out[4] = (uint8_t)(addr->address_id >> 16);
  out[5] = (uint8_t)(addr->address_id >> 24);
  out[6] = addr->version;
  out[7] = addr->device_type;
}

nartis_rf_address_t nartis_rf_address_from_bytes(const uint8_t in[8]) {
  nartis_rf_address_t a = {0};
  a.manufacturer_id = (uint16_t)in[0] | ((uint16_t)in[1] << 8);
  a.address_id = (uint32_t)in[2] | ((uint32_t)in[3] << 8) | ((uint32_t)in[4] << 16) | ((uint32_t)in[5] << 24);
  a.version = in[6];
  a.device_type = in[7];
  return a;
}

nartis_rf_address_t nartis_rf_address_derive(const char *ciu_serial, uint32_t seed) {
  nartis_rf_address_t a = {NARTIS_RF_MANUFACTURER_ID, 0, NARTIS_RF_VERSION, NARTIS_RF_DEVICE_TYPE_CIU};
  size_t len = strlen(ciu_serial);
  const char *p = len > 7 ? ciu_serial + len - 7 : ciu_serial;
  uint32_t digits = 0;
  for (; *p; ++p) {
    if (!isdigit((unsigned char)*p)) break;
    digits = digits * 10u + (uint32_t)(*p - '0');
  }
  uint8_t seed_byte = (uint8_t)(seed % 255u);
  a.address_id = ((digits >> 16) & 0xFF) | (((digits >> 8) & 0xFF) << 8) |
                 ((digits & 0xFF) << 16) | ((uint32_t)seed_byte << 24);
  return a;
}

int nartis_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
  if (!hex || !out) return NARTIS_ERR_ARG;
  for (size_t i = 0; i < out_len; ++i) {
    int hi = hex[2 * i], lo = hex[2 * i + 1];
    if (!hi || !lo) return NARTIS_ERR_ARG;
    hi = isdigit(hi) ? hi - '0' : (tolower(hi) >= 'a' && tolower(hi) <= 'f' ? tolower(hi) - 'a' + 10 : -1);
    lo = isdigit(lo) ? lo - '0' : (tolower(lo) >= 'a' && tolower(lo) <= 'f' ? tolower(lo) - 'a' + 10 : -1);
    if (hi < 0 || lo < 0) return NARTIS_ERR_ARG;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return NARTIS_OK;
}

void nartis_rf_init(nartis_rf_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->channel = 1;
  ctx->rssi_dbm = -70;
}

void nartis_rf_set_address(nartis_rf_ctx_t *ctx, nartis_rf_address_t addr) { ctx->address = addr; }
void nartis_rf_set_meter_address(nartis_rf_ctx_t *ctx, nartis_rf_address_t addr) { ctx->meter_address = addr; ctx->meter_address_set = true; }
void nartis_rf_set_aes_key(nartis_rf_ctx_t *ctx, const uint8_t key[16]) { memcpy(ctx->aes_key, key, 16); }
void nartis_rf_get_aes_key(const nartis_rf_ctx_t *ctx, uint8_t key[16]) { memcpy(key, ctx->aes_key, 16); }
void nartis_rf_set_channel_quality(nartis_rf_ctx_t *ctx, uint8_t ch, int8_t rssi) { ctx->channel = ch & 3; ctx->rssi_dbm = rssi; }

static uint8_t channel_byte(uint8_t channel_idx, int8_t rssi_dbm) {
  int q = ((int)rssi_dbm + 130) / 2;
  if (q < 1) q = 1;
  if (q > 62) q = 62;
  return (uint8_t)(((channel_idx & 3) << 6) | (q & 0x3F));
}

size_t nartis_rf_build_frame(nartis_rf_ctx_t *ctx, uint8_t *out, size_t out_max,
                             nartis_rf_frame_type_t type, uint8_t sequence,
                             const uint8_t *payload, size_t payload_len) {
  if (!ctx || !out || out_max < RF_TX_HDR_SIZE + 4) return 0;
  bool encrypted = type == RF_FRAME_ACK || type == RF_FRAME_BEACON;
  out[RF_TX_LENGTH] = 0;
  out[RF_TX_FLAGS] = (uint8_t)type;
  nartis_rf_address_to_bytes(&ctx->address, out + RF_TX_ADDR);
  out[RF_TX_CI_FIELD] = type == RF_FRAME_PLAIN_DATA ? CI_FIELD_SPECIAL : CI_FIELD_NORMAL;
  out[RF_TX_SEQUENCE] = sequence;
  out[RF_TX_CHANNEL_BYTE] = channel_byte(ctx->channel, ctx->rssi_dbm);
  out[RF_TX_ENC_FLAG] = encrypted ? 0x01 : 0x00;
  out[RF_TX_SEC_FLAG] = encrypted ? AES_GCM_FLAG : 0x00;
  size_t body_size = RF_TX_HDR_SIZE;
  if (encrypted) {
    if (payload_len > 255 || RF_TX_ENC_CIPHER + payload_len + 16 > out_max) return 0;
    if (!ctx->has_sent_first_tx || sequence != ctx->last_tx_sequence) {
      ctx->frame_counter++;
      ctx->last_tx_sequence = sequence;
      ctx->has_sent_first_tx = true;
    }
    out[RF_TX_ENC_DATA_LEN] = (uint8_t)payload_len;
    out[RF_TX_ENC_ZERO] = 0;
    out[RF_TX_ENC_COUNTER] = (uint8_t)(ctx->frame_counter >> 24);
    out[RF_TX_ENC_COUNTER + 1] = (uint8_t)(ctx->frame_counter >> 16);
    out[RF_TX_ENC_COUNTER + 2] = (uint8_t)(ctx->frame_counter >> 8);
    out[RF_TX_ENC_COUNTER + 3] = (uint8_t)ctx->frame_counter;
    memcpy(out + RF_TX_ENC_CIPHER, payload, payload_len);
    int enc_len = nartis_rf_encrypt(ctx, out + RF_TX_ENC_CIPHER, payload_len,
                                    out_max - RF_TX_ENC_CIPHER - 4, ctx->frame_counter);
    if (enc_len < 0) return 0;
    body_size = RF_TX_ENC_CIPHER + (size_t)enc_len;
  } else {
    if (RF_TX_HDR_SIZE + payload_len + 4 > out_max) return 0;
    if (payload_len) memcpy(out + RF_TX_HDR_SIZE, payload, payload_len);
    body_size = RF_TX_HDR_SIZE + payload_len;
  }
  size_t total = crc_insert(out, body_size, out_max);
  if (total) out[0] = (uint8_t)(total - 1);
  return total;
}

int nartis_rf_parse_frame(nartis_rf_ctx_t *ctx, const uint8_t *in, size_t in_len,
                          uint8_t *payload_out, size_t payload_max, uint8_t *type_out) {
  if (!ctx || !in || in_len < RF_RX_HDR_SIZE + 2) return NARTIS_ERR_PROTOCOL;
  size_t frame_size = (size_t)in[RF_RX_LENGTH] + 1;
  if (frame_size > in_len || frame_size > sizeof(ctx->work) || frame_size > 0x122) return NARTIS_ERR_PROTOCOL;
  memcpy(ctx->work, in, frame_size);
  int body_size = crc_strip(ctx->work, frame_size);
  if (body_size < 0) return NARTIS_ERR_CRC;
  if (type_out) *type_out = ctx->work[RF_RX_TYPE];
  if (ctx->meter_address_set) {
    /* Compare by the 8-byte wire form, not the struct: nartis_rf_address_t has
     * padding bytes that a struct memcmp would include. */
    uint8_t mb[8];
    nartis_rf_address_to_bytes(&ctx->meter_address, mb);
    if (memcmp(ctx->work + RF_RX_ADDR, mb, 8) != 0) return NARTIS_ERR_PROTOCOL;
  }
  uint8_t frame_type = ctx->work[RF_RX_TYPE];
  bool plain = frame_type == RX_TYPE_PRESENCE_ACK || frame_type == RX_TYPE_SHORT_ACK ||
               frame_type == RX_TYPE_DATA || frame_type == RX_TYPE_SESSION_SETUP ||
               frame_type == RX_TYPE_REQUEST_ACK;
  if (plain) {
    if (body_size <= RF_RX_PAYLOAD) return 0;
    size_t payload_len = (size_t)body_size - RF_RX_PAYLOAD;
    if (payload_len > payload_max) return NARTIS_ERR_ARG;
    memcpy(payload_out, ctx->work + RF_RX_PAYLOAD, payload_len);
    return (int)payload_len;
  }
  if (body_size < 19 + NARTIS_AES_TAG_SIZE) return NARTIS_ERR_PROTOCOL;
  uint8_t enc_len = ctx->work[13];
  if (ctx->work[14] != 0) return NARTIS_ERR_PROTOCOL;
  uint32_t counter = ((uint32_t)ctx->work[15] << 24) | ((uint32_t)ctx->work[16] << 16) |
                     ((uint32_t)ctx->work[17] << 8) | ctx->work[18];
  if ((size_t)enc_len + 0x1F != (size_t)body_size) return NARTIS_ERR_PROTOCOL;
  if (counter <= ctx->last_rx_counter) return NARTIS_ERR_REPLAY;
  int dec = nartis_rf_decrypt(ctx, ctx->work + 19, (size_t)enc_len + NARTIS_AES_TAG_SIZE, counter);
  if (dec < 0) return dec;
  if ((size_t)dec > payload_max) return NARTIS_ERR_ARG;
  ctx->last_rx_counter = counter;
  memcpy(payload_out, ctx->work + 19, (size_t)dec);
  return dec;
}

int nartis_rf_parse_nested(nartis_rf_ctx_t *ctx, const uint8_t *payload, size_t payload_len,
                           uint8_t *plain_out, size_t plain_max, bool mutate_counter) {
  if (!ctx || !payload || payload_len < 16 + NARTIS_AES_TAG_SIZE) return NARTIS_ERR_PROTOCOL;
  if (payload[8] != 0x01 || payload[9] != AES_GCM_FLAG || payload[11] != 0) return NARTIS_ERR_PROTOCOL;
  uint8_t enc_len = payload[10];
  uint32_t counter = ((uint32_t)payload[12] << 24) | ((uint32_t)payload[13] << 16) |
                     ((uint32_t)payload[14] << 8) | payload[15];
  if (16 + (size_t)enc_len + NARTIS_AES_TAG_SIZE > payload_len) return NARTIS_ERR_PROTOCOL;
  if (enc_len > plain_max || (size_t)enc_len + NARTIS_AES_TAG_SIZE > sizeof(ctx->work)) return NARTIS_ERR_ARG;
  if (mutate_counter && counter <= ctx->last_nested_rx_counter) return NARTIS_ERR_REPLAY;
  memcpy(ctx->work, payload + 16, (size_t)enc_len + NARTIS_AES_TAG_SIZE);
  int dec = nartis_rf_decrypt(ctx, ctx->work, (size_t)enc_len + NARTIS_AES_TAG_SIZE, counter);
  if (dec < 0) return dec;
  if (mutate_counter) ctx->last_nested_rx_counter = counter;
  memcpy(plain_out, ctx->work, (size_t)dec);
  return dec;
}

