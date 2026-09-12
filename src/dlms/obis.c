#include "internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nartis_obis_parse(const char *text, nartis_obis_t *out) {
  if (!text || !out) return NARTIS_ERR_ARG;
  unsigned vals[6] = {0};
  int count = 0;
  const char *p = text;
  while (*p && count < 6) {
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) break;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || v > 255) return NARTIS_ERR_ARG;
    vals[count++] = (unsigned)v;
    p = end;
  }
  if (count != 6) return NARTIS_ERR_ARG;
  for (int i = 0; i < 6; ++i) out->bytes[i] = (uint8_t)vals[i];
  return NARTIS_OK;
}

void nartis_obis_format(const nartis_obis_t *obis, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  if (!obis) {
    out[0] = '\0';
    return;
  }
  snprintf(out, out_size, "%u.%u.%u.%u.%u.%u",
           obis->bytes[0], obis->bytes[1], obis->bytes[2],
           obis->bytes[3], obis->bytes[4], obis->bytes[5]);
}

