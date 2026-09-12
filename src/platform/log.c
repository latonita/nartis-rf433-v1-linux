#include "nartis/nartis_backend.h"
#include <stdarg.h>
#include <stdio.h>

void nartis_backend_log(const nartis_backend_t *backend, nartis_log_level_t level,
                        const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (backend && backend->vlog) {
    backend->vlog(backend->ctx, level, fmt, ap);
  } else {
    static const char *names[] = {"ERR", "WRN", "INF", "DBG", "TRC"};
    fprintf(stderr, "[%s] ", names[level >= 0 && level <= NARTIS_LOG_TRACE ? level : 0]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
  }
  va_end(ap);
}

const char *nartis_strerror(int status) {
  switch (status) {
    case NARTIS_OK: return "ok";
    case NARTIS_ERR_ARG: return "invalid argument";
    case NARTIS_ERR_IO: return "I/O error";
    case NARTIS_ERR_TIMEOUT: return "timeout";
    case NARTIS_ERR_PROTOCOL: return "protocol error";
    case NARTIS_ERR_CRC: return "CRC error";
    case NARTIS_ERR_CRYPTO: return "crypto error";
    case NARTIS_ERR_REPLAY: return "replayed frame";
    case NARTIS_ERR_UNSUPPORTED: return "unsupported";
    case NARTIS_ERR_STATE: return "invalid state";
    default: return "unknown error";
  }
}

