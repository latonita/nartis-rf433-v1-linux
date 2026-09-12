#include "internal.h"
#include <openssl/evp.h>
#include <string.h>

static void build_nonce(const nartis_rf_ctx_t *ctx, uint32_t counter, uint8_t nonce[12]) {
  uint8_t aw[8];
  static const uint8_t perm[8] = {2, 3, 4, 5, 0, 1, 6, 7};
  nartis_rf_address_to_bytes(&ctx->address, aw);
  for (int i = 0; i < 8; ++i) nonce[i] = aw[perm[i]];
  nonce[8] = (uint8_t)(counter >> 24);
  nonce[9] = (uint8_t)(counter >> 16);
  nonce[10] = (uint8_t)(counter >> 8);
  nonce[11] = (uint8_t)counter;
}

int nartis_gcm_crypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t aad[4],
                     uint8_t *data, size_t len, uint8_t tag[12], bool encrypt) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return NARTIS_ERR_CRYPTO;
  int ok = 0, outl = 0;
  if (encrypt) {
    ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
         EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
         EVP_EncryptUpdate(ctx, NULL, &outl, aad, 4) == 1 &&
         EVP_EncryptUpdate(ctx, data, &outl, data, (int)len) == 1 &&
         EVP_EncryptFinal_ex(ctx, data + outl, &outl) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 12, tag) == 1;
  } else {
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
         EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
         EVP_DecryptUpdate(ctx, NULL, &outl, aad, 4) == 1 &&
         EVP_DecryptUpdate(ctx, data, &outl, data, (int)len) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 12, tag) == 1 &&
         EVP_DecryptFinal_ex(ctx, data + outl, &outl) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok ? NARTIS_OK : NARTIS_ERR_CRYPTO;
}

int nartis_rf_encrypt(nartis_rf_ctx_t *ctx, uint8_t *data, size_t len, size_t buf_max,
                      uint32_t counter) {
  if (len + 12 > buf_max) return NARTIS_ERR_ARG;
  uint8_t nonce[12];
  build_nonce(ctx, counter, nonce);
  uint8_t aad[4] = {0x01, 0x29, (uint8_t)len, 0x00};
  int rc = nartis_gcm_crypt(ctx->aes_key, nonce, aad, data, len, data + len, true);
  return rc == NARTIS_OK ? (int)(len + 12) : rc;
}

int nartis_rf_decrypt_with_key(const nartis_rf_ctx_t *ctx, const uint8_t key[16],
                               uint8_t *data, size_t len_with_tag, uint32_t counter) {
  if (len_with_tag < 12) return NARTIS_ERR_ARG;
  size_t ct_len = len_with_tag - 12;
  uint8_t nonce[12];
  build_nonce(ctx, counter, nonce);
  uint8_t aad[4] = {0x01, 0x29, (uint8_t)ct_len, 0x00};
  return nartis_gcm_crypt(key, nonce, aad, data, ct_len, data + ct_len, false) == NARTIS_OK
             ? (int)ct_len
             : NARTIS_ERR_CRYPTO;
}

int nartis_rf_decrypt(nartis_rf_ctx_t *ctx, uint8_t *data, size_t len_with_tag, uint32_t counter) {
  return nartis_rf_decrypt_with_key(ctx, ctx->aes_key, data, len_with_tag, counter);
}

bool nartis_rf_extract_session_key(const nartis_rf_ctx_t *ctx, const uint8_t *payload,
                                   size_t len, uint8_t out_key[16]) {
  static const uint8_t marker[4] = {0x0D, 0xFD, 0x19, 0x1E};
  static const uint8_t factory[16] = {
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22
  };
  for (size_t h = 0; h + 26 <= len; ++h) {
    if (memcmp(payload + h, marker, 4) != 0) continue;
    const uint8_t *c = payload + h + 4;
    uint32_t counter = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
    uint8_t nonce[12];
    build_nonce(ctx, counter, nonce);
    uint8_t buf[18];
    memcpy(buf, payload + h + 8, sizeof(buf));
    uint8_t throwaway[12] = {0};
    uint8_t aad[4] = {0};
    if (nartis_gcm_crypt(factory, nonce, aad, buf, sizeof(buf), throwaway, true) != NARTIS_OK) return false;
    if (buf[1] != 16) return false;
    memcpy(out_key, buf + 2, 16);
    return true;
  }
  return false;
}

