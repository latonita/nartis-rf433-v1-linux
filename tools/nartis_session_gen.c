/* Write a Nartis session file from explicit values, so a device can start
 * already paired (session transplant) instead of pairing fresh. Uses the
 * library's own store routine, so the on-disk format never drifts. */
#include "internal.h"
#include "nartis/nartis_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *argval(int *i, int argc, char **argv) {
  if (*i + 1 >= argc) return NULL;
  (*i)++;
  return argv[*i];
}

static int set_hex(const char *hex, uint8_t *out, size_t n, const char *name) {
  if (nartis_hex_to_bytes(hex, out, n) != NARTIS_OK) {
    fprintf(stderr, "bad %s (need %zu hex bytes): %s\n", name, n, hex ? hex : "(null)");
    return -1;
  }
  return 0;
}

static void usage(void) {
  fprintf(stderr,
    "Usage: nartis_session_gen --key HEX16 --meter HEX8 --ciu HEX8 [--out PATH]\n"
    "       [--frame-counter N] [--last-rx N] [--nested-rx N] [--data-seq N] [--beacon-seq N]\n"
    "\n"
    "  --key HEX16   16-byte AES session key as 32 hex chars (meter serial ASCII + 4 assigned bytes)\n"
    "  --meter HEX8  meter RF address, 16 hex chars\n"
    "  --ciu HEX8    CIU RF address, 16 hex chars (must match --ciu-address used at read time)\n"
    "  --frame-counter N  set above the source device's last TX counter to avoid replay\n"
    "\n"
    "Writes a session file the library can load so a device starts already paired.\n"
    "Obtain the key/addresses/counters from an already-paired device's session.\n");
}

int main(int argc, char **argv) {
  const char *out_path = "meter.session";
  const char *key = NULL;
  const char *meter = NULL;
  const char *ciu = NULL;
  uint32_t frame_counter = 100;
  uint32_t last_rx = 0;
  uint32_t nested_rx = 0;
  unsigned data_seq = 1;
  unsigned beacon_seq = 1;

  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (strcmp(a, "--out") == 0) out_path = argval(&i, argc, argv);
    else if (strcmp(a, "--key") == 0) key = argval(&i, argc, argv);
    else if (strcmp(a, "--meter") == 0) meter = argval(&i, argc, argv);
    else if (strcmp(a, "--ciu") == 0) ciu = argval(&i, argc, argv);
    else if (strcmp(a, "--frame-counter") == 0) frame_counter = (uint32_t) strtoul(argval(&i, argc, argv), NULL, 0);
    else if (strcmp(a, "--last-rx") == 0) last_rx = (uint32_t) strtoul(argval(&i, argc, argv), NULL, 0);
    else if (strcmp(a, "--nested-rx") == 0) nested_rx = (uint32_t) strtoul(argval(&i, argc, argv), NULL, 0);
    else if (strcmp(a, "--data-seq") == 0) data_seq = (unsigned) strtoul(argval(&i, argc, argv), NULL, 0);
    else if (strcmp(a, "--beacon-seq") == 0) beacon_seq = (unsigned) strtoul(argval(&i, argc, argv), NULL, 0);
    else { usage(); return 2; }
  }

  if (!key || !meter || !ciu) {
    fprintf(stderr, "--key, --meter and --ciu are required\n");
    usage();
    return 2;
  }

  nartis_session_state_t st;
  memset(&st, 0, sizeof(st));
  st.version = 1;
  if (set_hex(key, st.aes_key, 16, "--key") != 0) return 2;
  if (set_hex(meter, st.meter_addr, 8, "--meter") != 0) return 2;
  if (set_hex(ciu, st.ciu_addr, 8, "--ciu") != 0) return 2;
  st.frame_counter = frame_counter;
  st.last_rx_counter = last_rx;
  st.last_nested_rx_counter = nested_rx;
  st.data_seq = (uint8_t) data_seq;
  st.beacon_seq = (uint8_t) beacon_seq;
  st.paired = 1;
  st.session_primed = 0;

  int rc = nartis_session_store_save(out_path, &st);
  if (rc != NARTIS_OK) {
    fprintf(stderr, "save failed: %s (%d)\n", nartis_strerror(rc), rc);
    return 1;
  }

  /* Round-trip check. */
  nartis_session_state_t rb;
  rc = nartis_session_store_load(out_path, &rb);
  if (rc != NARTIS_OK || memcmp(&rb, &st, sizeof(st)) != 0) {
    fprintf(stderr, "round-trip verify FAILED: %s (%d)\n", nartis_strerror(rc), rc);
    return 1;
  }

  printf("wrote %s: ciu=%s meter=%s frame_counter=%u (round-trip OK)\n",
         out_path, ciu, meter, frame_counter);
  return 0;
}
