#include "nartis/nartis.h"
#include "internal.h"
#include <stdio.h>
#include <string.h>

static int fail(const char *msg) {
  fprintf(stderr, "FAIL: %s\n", msg);
  return 1;
}

int main(void) {
  nartis_obis_t obis;
  if (nartis_obis_parse("1.0.32.7.0.255", &obis) != NARTIS_OK) return fail("obis parse");
  if (obis.bytes[0] != 1 || obis.bytes[2] != 32 || obis.bytes[5] != 255) return fail("obis bytes");

  uint8_t sample[] = {0x2d, 0x46, 0xcd, 0x2c, 0, 0, 1, 2, 0x50, 0x25};
  uint16_t c = nartis_crc16_dnp(sample, sizeof(sample));
  uint8_t with_crc[sizeof(sample) + 2];
  memcpy(with_crc, sample, sizeof(sample));
  with_crc[sizeof(sample)] = (uint8_t)(c >> 8);
  with_crc[sizeof(sample) + 1] = (uint8_t)c;
  if (!nartis_crc16_verify(with_crc, sizeof(with_crc))) return fail("crc verify");

  nartis_attr_t a = {.class_id = 3, .attr_id = 2};
  a.obis = obis;
  uint8_t apdu[32];
  size_t n = nartis_dlms_build_get_request_with_list(apdu, sizeof(apdu), &a, 1, 1);
  if (n != 14 || apdu[0] != 0xC0 || apdu[1] != 0x03 || apdu[3] != 1) return fail("dlms request");

  nartis_rf_ctx_t rf;
  nartis_rf_init(&rf);
  nartis_rf_set_address(&rf, nartis_rf_address_derive("MPCUA00294W6", 230));
  uint8_t key[16] = "012345678901";
  key[12] = 0xBD; key[13] = 0x02; key[14] = 0x9B; key[15] = 0xBE;
  nartis_rf_set_aes_key(&rf, key);
  uint8_t frame[256];
  uint8_t payload[] = {0x0D, 0xFD, 0x0D, 0x04};
  size_t flen = nartis_rf_build_frame(&rf, frame, sizeof(frame), RF_FRAME_BEACON, 1, payload, sizeof(payload));
  if (flen == 0 || frame[1] != 0x08 || frame[13] != 1 || !nartis_crc16_verify(frame, flen)) return fail("rf build");

  /* get-response-with-list: IEC wrapper (version 00 01, wPorts, length) +
   * C4 03 <invoke> <count> + one result: success, uint16 2344 */
  uint8_t resp[] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x00, 0x08,
                    0xC4, 0x03, 0xC2, 0x01, 0x00, 0x12, 0x09, 0x28};
  nartis_value_t vals[NARTIS_MAX_LIST_ATTRS];
  uint8_t got = 0;
  int inv = -1;
  if (!nartis_dlms_parse_read_response_list(resp, sizeof(resp), vals, NARTIS_MAX_LIST_ATTRS, &got,
                                            NULL, 2, true, &inv) ||
      got != 1 || inv != 2 || !vals[0].valid || nartis_value_as_double(&vals[0]) != 2344.0)
    return fail("strict parse: matching invoke-id accepted");
  if (nartis_dlms_parse_read_response_list(resp, sizeof(resp), vals, NARTIS_MAX_LIST_ATTRS, &got,
                                           NULL, 3, true, &inv) || inv != 2)
    return fail("strict parse: invoke-id mismatch rejected");
  if (!nartis_dlms_parse_read_response_list(resp, sizeof(resp), vals, NARTIS_MAX_LIST_ATTRS, &got,
                                            NULL, -1, false, &inv) || got != 1)
    return fail("lenient parse: accepted without invoke check");
  uint8_t short_resp[sizeof(resp)];
  memcpy(short_resp, resp, sizeof(resp));
  short_resp[11] = 0x02;  /* declares 2 results, carries 1 */
  if (nartis_dlms_parse_read_response_list(short_resp, sizeof(short_resp), vals,
                                           NARTIS_MAX_LIST_ATTRS, &got, NULL, 2, true, &inv))
    return fail("strict parse: result count mismatch rejected");
  uint8_t long_resp[sizeof(resp) + 1];
  memcpy(long_resp, resp, sizeof(resp));
  long_resp[sizeof(resp)] = 0x00;  /* trailing byte vs declared IEC length */
  if (nartis_dlms_parse_read_response_list(long_resp, sizeof(long_resp), vals,
                                           NARTIS_MAX_LIST_ATTRS, &got, NULL, 2, true, &inv))
    return fail("strict parse: IEC length mismatch rejected");

  puts("OK");
  return 0;
}

