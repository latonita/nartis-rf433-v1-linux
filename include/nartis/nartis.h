#ifndef NARTIS_H
#define NARTIS_H

#include "cmt2300a.h"
#include "nartis_backend.h"
#include "nartis_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nartis_meter nartis_meter_t;

typedef struct {
  nartis_cmt2300a_pins_t pins;
  const char *meter_serial;
  const char *ciu_serial;
  const char *ciu_address_hex;
  int fixed_channel;
  bool use_alternative_channels;
  uint8_t batch_size;
  uint32_t rx_timeout_ms;
  uint32_t rx_reply_timeout_ms;
  const char *session_path;
} nartis_config_t;

int nartis_open(nartis_meter_t **out, const nartis_config_t *config,
                const nartis_backend_t *backend);
void nartis_close(nartis_meter_t *meter);
int nartis_init(nartis_meter_t *meter);
int nartis_pair(nartis_meter_t *meter);
int nartis_repair(nartis_meter_t *meter);
int nartis_read(nartis_meter_t *meter, const nartis_attr_t *attrs, size_t attr_count,
                nartis_value_t *values, size_t value_capacity);
int nartis_scan_channels(nartis_meter_t *meter, int8_t scores[4], uint8_t *best_ch);
int nartis_save_session(nartis_meter_t *meter);
int nartis_load_session(nartis_meter_t *meter);
const nartis_session_state_t *nartis_session(const nartis_meter_t *meter);

#ifdef __cplusplus
}
#endif

#endif

