#include "internal.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

size_t nartis_dlms_build_get_request_with_list(uint8_t *out, size_t max,
                                               const nartis_attr_t *attrs, uint8_t count) {
  if (!out || !attrs || count == 0 || count > NARTIS_MAX_LIST_ATTRS) return 0;
  size_t needed = 4u + 10u * count;
  if (max < needed) return 0;
  size_t pos = 0;
  out[pos++] = 0xC0;
  out[pos++] = 0x03;
  out[pos++] = 0xC1;
  out[pos++] = count;
  for (uint8_t i = 0; i < count; ++i) {
    out[pos++] = (uint8_t)(attrs[i].class_id >> 8);
    out[pos++] = (uint8_t)attrs[i].class_id;
    memcpy(out + pos, attrs[i].obis.bytes, 6);
    pos += 6;
    out[pos++] = attrs[i].attr_id;
    out[pos++] = 0x00;
  }
  return pos;
}

size_t nartis_dlms_build_get_request_normal(uint8_t *out, size_t max,
                                            const nartis_obis_t *obis,
                                            uint16_t class_id, uint8_t attr_id) {
  if (!out || !obis || max < 13) return 0;
  size_t pos = 0;
  out[pos++] = 0xC0;
  out[pos++] = 0x01;
  out[pos++] = 0xC1;
  out[pos++] = (uint8_t)(class_id >> 8);
  out[pos++] = (uint8_t)class_id;
  memcpy(out + pos, obis->bytes, 6);
  pos += 6;
  out[pos++] = attr_id;
  out[pos++] = 0x00;
  return pos;
}

static int skip_typed_value(const uint8_t *data, size_t len) {
  if (len < 1) return -1;
  uint8_t type = data[0];
  size_t pos = 1;
  switch (type) {
    case 0x00: return 1;
    case 0x03:
    case 0x0F:
    case 0x11:
    case 0x16:
    case 0x04:
      return len < 2 ? -1 : 2;
    case 0x10:
    case 0x12:
      return len < 3 ? -1 : 3;
    case 0x05:
    case 0x06:
    case 0x17:
      return len < 5 ? -1 : 5;
    case 0x14:
    case 0x15:
    case 0x18:
      return len < 9 ? -1 : 9;
    case 0x09:
    case 0x0A:
    case 0x0C:
    case 0x0D: {
      if (len < 2) return -1;
      uint8_t slen = data[pos++];
      return pos + slen > len ? -1 : (int)(pos + slen);
    }
    case 0x01:
    case 0x02: {
      if (len < 2) return -1;
      uint8_t count = data[pos++];
      for (uint8_t i = 0; i < count; ++i) {
        int n = skip_typed_value(data + pos, len - pos);
        if (n < 0) return -1;
        pos += (size_t)n;
      }
      return (int)pos;
    }
    case 0x19: return len < 13 ? -1 : 13;
    case 0x1A: return len < 6 ? -1 : 6;
    case 0x1B: return len < 5 ? -1 : 5;
    default: return -1;
  }
}

static int parse_typed_value(const uint8_t *data, size_t len, nartis_value_t *value,
                             uint16_t class_hint) {
  if (!data || !value || len < 1) return -1;
  uint8_t type = data[0];
  size_t hdr = 1, vlen = 0;
  switch (type) {
    case NARTIS_DLMS_NULL_DATA:
      memset(value, 0, sizeof(*value));
      value->dtype = type;
      return 1;
    case NARTIS_DLMS_BOOLEAN:
    case NARTIS_DLMS_ENUM:
    case NARTIS_DLMS_UINT8:
    case NARTIS_DLMS_INT8:
    case NARTIS_DLMS_BIT_STRING:
      vlen = 1;
      break;
    case NARTIS_DLMS_UINT16:
    case NARTIS_DLMS_INT16:
      vlen = 2;
      break;
    case NARTIS_DLMS_TIME:
      vlen = 4;
      break;
    case NARTIS_DLMS_DATE:
      vlen = 5;
      break;
    case NARTIS_DLMS_UINT32:
    case NARTIS_DLMS_INT32:
    case NARTIS_DLMS_FLOAT32:
      vlen = 4;
      break;
    case NARTIS_DLMS_UINT64:
    case NARTIS_DLMS_INT64:
    case NARTIS_DLMS_FLOAT64:
      vlen = 8;
      break;
    case NARTIS_DLMS_DATE_TIME:
      vlen = 12;
      break;
    case NARTIS_DLMS_OCTET_STRING:
    case NARTIS_DLMS_VISIBLE_STRING:
    case NARTIS_DLMS_UTF8_STRING:
    case NARTIS_DLMS_BCD:
      if (len < 2) return -1;
      vlen = data[1];
      hdr = 2;
      break;
    default:
      value->valid = false;
      return -1;
  }
  if (len < hdr + vlen) return -1;
  size_t copy = vlen > sizeof(value->raw) ? sizeof(value->raw) : vlen;
  memcpy(value->raw, data + hdr, copy);
  value->raw_len = (uint8_t)copy;
  value->dtype = (class_hint == 8 && type == NARTIS_DLMS_OCTET_STRING && vlen >= 12)
                   ? NARTIS_DLMS_DATE_TIME
                   : type;
  value->valid = true;
  return (int)(hdr + vlen);
}

bool nartis_dlms_parse_read_response_list(const uint8_t *data, size_t len,
                                          nartis_value_t *values, uint8_t max_results,
                                          uint8_t *count_out,
                                          const uint16_t *class_ids) {
  if (count_out) *count_out = 0;
  if (!data || !values || len < 6) return false;
  size_t pos = 0;
  if (len >= 5 && data[0] == 0x0D && data[1] == 0xFD && data[2] == 0xF8) pos = 5;
  if (len < pos + 8 || data[pos] != 0x00 || data[pos + 1] != 0x01) return false;
  pos += 8;
  if (len < pos + 4 || data[pos] != 0xC4 || data[pos + 1] != 0x03) return false;
  uint8_t count = data[pos + 3];
  pos += 4;
  uint8_t stored = 0;
  for (uint8_t i = 0; i < count && pos < len; ++i) {
    uint8_t result = data[pos++];
    if (result == 0x01) {
      if (pos >= len) break;
      pos++;
      if (stored < max_results) {
        memset(&values[stored], 0, sizeof(values[stored]));
        stored++;
      }
      continue;
    }
    if (result != 0x00 || pos >= len) break;
    uint8_t type = data[pos];
    bool compound = type == 0x01 || type == 0x02;
    if (!compound && stored < max_results) {
      uint16_t hint = class_ids && stored < max_results ? class_ids[stored] : 0;
      int n = parse_typed_value(data + pos, len - pos, &values[stored], hint);
      if (n < 0) {
        int s = skip_typed_value(data + pos, len - pos);
        if (s < 0) break;
        pos += (size_t)s;
        memset(&values[stored], 0, sizeof(values[stored]));
      } else {
        pos += (size_t)n;
      }
      stored++;
    } else {
      int s = skip_typed_value(data + pos, len - pos);
      if (s < 0) break;
      pos += (size_t)s;
      if (stored < max_results) {
        memset(&values[stored], 0, sizeof(values[stored]));
        stored++;
      }
    }
  }
  if (count_out) *count_out = stored;
  return true;
}

double nartis_value_as_double(const nartis_value_t *v) {
  if (!v || !v->valid || v->raw_len == 0) return 0.0;
  const uint8_t *p = v->raw;
  switch (v->dtype) {
    case NARTIS_DLMS_BOOLEAN:
    case NARTIS_DLMS_ENUM:
    case NARTIS_DLMS_UINT8:
    case NARTIS_DLMS_BIT_STRING:
      return p[0];
    case NARTIS_DLMS_INT8:
      return (int8_t)p[0];
    case NARTIS_DLMS_UINT16:
      return v->raw_len >= 2 ? be16(p) : 0.0;
    case NARTIS_DLMS_INT16:
      return v->raw_len >= 2 ? (int16_t)be16(p) : 0.0;
    case NARTIS_DLMS_UINT32:
      return v->raw_len >= 4 ? be32(p) : 0.0;
    case NARTIS_DLMS_INT32:
      return v->raw_len >= 4 ? (int32_t)be32(p) : 0.0;
    case NARTIS_DLMS_UINT64:
      return v->raw_len >= 8 ? (double)be64(p) : 0.0;
    case NARTIS_DLMS_INT64:
      return v->raw_len >= 8 ? (double)(int64_t)be64(p) : 0.0;
    case NARTIS_DLMS_FLOAT32: {
      if (v->raw_len < 4) return 0.0;
      uint32_t x = be32(p);
      float f;
      memcpy(&f, &x, sizeof(f));
      return f;
    }
    case NARTIS_DLMS_FLOAT64: {
      if (v->raw_len < 8) return 0.0;
      uint64_t x = be64(p);
      double d;
      memcpy(&d, &x, sizeof(d));
      return d;
    }
    default:
      return 0.0;
  }
}

static void hex_of(const uint8_t *data, size_t len, char *out, size_t out_size) {
  size_t pos = 0;
  if (out_size) out[0] = '\0';
  for (size_t i = 0; i < len && pos + 2 < out_size; ++i) {
    int n = snprintf(out + pos, out_size - pos, "%02x", data[i]);
    if (n > 0) pos += (size_t)n;
  }
}

static void format_cosem_datetime(const uint8_t *data, size_t len, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (len < 12) return;
  uint16_t year = be16(data);
  uint8_t month = data[2], day = data[3], hour = data[5], minute = data[6], second = data[7];
  int16_t dev = (int16_t)be16(data + 9);
  char y[5], mo[3], da[3], h[3], mi[3], s[3];
  snprintf(y, sizeof(y), (year != 0 && year != 0xFFFF) ? "%04u" : "????", year);
  snprintf(mo, sizeof(mo), (month >= 1 && month <= 12) ? "%02u" : "??", month);
  snprintf(da, sizeof(da), (day >= 1 && day <= 31) ? "%02u" : "??", day);
  snprintf(h, sizeof(h), hour <= 23 ? "%02u" : "??", hour);
  snprintf(mi, sizeof(mi), minute <= 59 ? "%02u" : "??", minute);
  snprintf(s, sizeof(s), second <= 59 ? "%02u" : "??", second);
  if (dev != (int16_t)0x8000) {
    int absd = dev >= 0 ? dev : -dev;
    snprintf(out, out_size, "%s-%s-%s %s:%s:%s %c%02d:%02d",
             y, mo, da, h, mi, s, dev >= 0 ? '+' : '-', absd / 60, absd % 60);
  } else {
    snprintf(out, out_size, "%s-%s-%s %s:%s:%s", y, mo, da, h, mi, s);
  }
}

void nartis_value_to_string(const nartis_value_t *v, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!v || !v->valid) return;
  switch (v->dtype) {
    case NARTIS_DLMS_OCTET_STRING:
    case NARTIS_DLMS_VISIBLE_STRING:
    case NARTIS_DLMS_UTF8_STRING: {
      size_t eff = v->raw_len;
      while (eff > 0 && v->raw[eff - 1] == 0) eff--;
      size_t pos = 0;
      for (size_t i = 0; i < eff && pos + 1 < out_size; ++i) out[pos++] = v->raw[i] >= 0x20 ? (char)v->raw[i] : '.';
      out[pos] = '\0';
      break;
    }
    case NARTIS_DLMS_DATE_TIME:
      format_cosem_datetime(v->raw, v->raw_len, out, out_size);
      break;
    case NARTIS_DLMS_BIT_STRING:
    case NARTIS_DLMS_BCD:
    case NARTIS_DLMS_DATE:
    case NARTIS_DLMS_TIME:
      hex_of(v->raw, v->raw_len, out, out_size);
      break;
    default:
      snprintf(out, out_size, "%.17g", nartis_value_as_double(v));
      break;
  }
}

