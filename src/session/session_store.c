#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint32_t magic;
  uint32_t size;
  nartis_session_state_t state;
  uint32_t crc;
} file_state_t;

enum { STORE_MAGIC = 0x4E415254u, STORE_VERSION = 1 };

static uint32_t fnv1a(const void *data, size_t len) {
  const uint8_t *p = data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

int nartis_session_store_load(const char *path, nartis_session_state_t *state) {
  if (!path || !state) return NARTIS_ERR_ARG;
  FILE *f = fopen(path, "rb");
  if (!f) return errno == ENOENT ? NARTIS_ERR_STATE : NARTIS_ERR_IO;
  file_state_t fs;
  size_t n = fread(&fs, 1, sizeof(fs), f);
  fclose(f);
  if (n != sizeof(fs) || fs.magic != STORE_MAGIC || fs.size != sizeof(fs.state)) return NARTIS_ERR_PROTOCOL;
  uint32_t crc = fnv1a(&fs.state, sizeof(fs.state));
  if (crc != fs.crc || fs.state.version != STORE_VERSION) return NARTIS_ERR_PROTOCOL;
  *state = fs.state;
  return NARTIS_OK;
}

int nartis_session_store_save(const char *path, const nartis_session_state_t *state) {
  if (!path || !state) return NARTIS_ERR_ARG;
  file_state_t fs;
  fs.magic = STORE_MAGIC;
  fs.size = sizeof(fs.state);
  fs.state = *state;
  fs.state.version = STORE_VERSION;
  fs.crc = fnv1a(&fs.state, sizeof(fs.state));
  FILE *f = fopen(path, "wb");
  if (!f) return NARTIS_ERR_IO;
  size_t n = fwrite(&fs, 1, sizeof(fs), f);
  int rc = ferror(f) ? NARTIS_ERR_IO : NARTIS_OK;
  fclose(f);
  return n == sizeof(fs) ? rc : NARTIS_ERR_IO;
}

