#include "ota_signature.h"

#include <memory>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/md5/md5.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

namespace esphome {
namespace ota_signature {

static const char *const TAG = "ota_signature";

// Inactivity timeout while streaming the image (no bytes for this long -> fail).
static const uint32_t READ_TIMEOUT_MS = 15000;
// Sanity cap; an ESP32 OTA image is well under this.
static const size_t MAX_FIRMWARE_SIZE = 8 * 1024 * 1024;
static const size_t CHUNK_SIZE = 1024;

// Strict base64 decoder (standard alphabet, mandatory padding). Returns the
// number of decoded bytes, or -1 on any malformed input.
static int base64_decode_strict(const std::string &in, uint8_t *out, size_t out_max) {
  auto decode_char = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return -1;
  };

  if (in.empty() || in.size() % 4 != 0)
    return -1;
  size_t out_len = 0;
  for (size_t i = 0; i < in.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (size_t j = 0; j < 4; j++) {
      char c = in[i + j];
      if (c == '=') {
        // Padding is only valid in the last two positions of the final group.
        if (i + 4 != in.size() || j < 2)
          return -1;
        v[j] = 0;
        pad++;
      } else {
        if (pad > 0)  // data after padding
          return -1;
        v[j] = decode_char(c);
        if (v[j] < 0)
          return -1;
      }
    }
    uint8_t bytes[3] = {
        (uint8_t) ((v[0] << 2) | (v[1] >> 4)),
        (uint8_t) (((v[1] & 0x0F) << 4) | (v[2] >> 2)),
        (uint8_t) (((v[2] & 0x03) << 6) | v[3]),
    };
    size_t n = 3 - pad;
    if (out_len + n > out_max)
      return -1;
    for (size_t j = 0; j < n; j++)
      out[out_len++] = bytes[j];
  }
  return (int) out_len;
}

void OtaSignatureVerifier::dump_config() {
  ESP_LOGCONFIG(TAG, "OTA Signature Verifier (Ed25519):");
  ESP_LOGCONFIG(TAG, "  Public key: %02x%02x%02x%02x... (fingerprint)", this->public_key_[0], this->public_key_[1],
                this->public_key_[2], this->public_key_[3]);
  bool all_zero = true;
  for (uint8_t b : this->public_key_) {
    if (b != 0)
      all_zero = false;
  }
  if (all_zero) {
    ESP_LOGE(TAG, "  Public key is the all-zero PLACEHOLDER - every OTA update will be REFUSED!");
  }
}

bool OtaSignatureVerifier::fail_(const std::string &reason) {
  this->last_error_ = reason;
  this->verified_md5_.clear();
  ESP_LOGE(TAG, "Firmware signature verification FAILED: %s", reason.c_str());
  return false;
}

bool OtaSignatureVerifier::verify_url(const std::string &url, const std::string &signature_b64) {
  this->verified_md5_.clear();
  this->last_error_.clear();

  bool all_zero = true;
  for (uint8_t b : this->public_key_) {
    if (b != 0)
      all_zero = false;
  }
  if (all_zero)
    return this->fail_("public key not configured (placeholder build)");
  if (this->http_ == nullptr)
    return this->fail_("http_request component not set");
  if (signature_b64.empty())
    return this->fail_("no signature provided by server");

  uint8_t signature[64];
  int sig_len = base64_decode_strict(signature_b64, signature, sizeof(signature));
  if (sig_len != 64)
    return this->fail_("malformed signature (expected base64 of 64 raw bytes)");

  ESP_LOGI(TAG, "Downloading %s for signature verification...", url.c_str());
  auto container = this->http_->get(url, {});
  if (container == nullptr)
    return this->fail_("HTTP connection failed");
  if (container->status_code != 200) {
    char buf[48];
    snprintf(buf, sizeof(buf), "HTTP status %d", container->status_code);
    container->end();
    return this->fail_(buf);
  }
  size_t total = container->content_length;
  if (total == 0 || total > MAX_FIRMWARE_SIZE) {
    container->end();
    return this->fail_("invalid content length");
  }

  // Ed25519 verification hash: SHA-512(R || A || M), streamed over M.
  crypto_sha512_ctx sha_ctx;
  crypto_sha512_init(&sha_ctx);
  crypto_sha512_update(&sha_ctx, signature, 32);            // R
  crypto_sha512_update(&sha_ctx, this->public_key_.data(), 32);  // A

  md5::MD5Digest md5_digest;
  md5_digest.init();

  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[CHUNK_SIZE]);
  if (!buf) {
    container->end();
    return this->fail_("out of memory");
  }

  size_t bytes_read = 0;
  size_t last_log = 0;
  uint32_t last_data_time = millis();
  bool read_error = false;
  std::string read_error_reason;

  while (bytes_read < total) {
    int n = container->read(buf.get(), std::min(CHUNK_SIZE, total - bytes_read));
    App.feed_wdt();
    yield();
    if (n < 0) {
      read_error = true;
      read_error_reason = "connection error while downloading";
      break;
    }
    if (n == 0) {
      if (millis() - last_data_time >= READ_TIMEOUT_MS) {
        read_error = true;
        read_error_reason = "download timed out";
        break;
      }
      delay(1);
      continue;
    }
    last_data_time = millis();
    crypto_sha512_update(&sha_ctx, buf.get(), (size_t) n);
    md5_digest.add(buf.get(), (size_t) n);
    bytes_read += (size_t) n;
    if (bytes_read - last_log >= 128 * 1024) {
      last_log = bytes_read;
      ESP_LOGD(TAG, "Verifying: %u / %u bytes", (unsigned) bytes_read, (unsigned) total);
    }
  }
  container->end();

  if (read_error)
    return this->fail_(read_error_reason);
  if (bytes_read != total)
    return this->fail_("short download");

  uint8_t hash[64];
  crypto_sha512_final(&sha_ctx, hash);
  uint8_t h_ram[32];
  crypto_eddsa_reduce(h_ram, hash);

  App.feed_wdt();
  // Constant-time-enough for a public verification; vartime is fine here as
  // all inputs (image, signature, public key) are public values.
  int rc = crypto_eddsa_check_equation(signature, this->public_key_.data(), h_ram);
  App.feed_wdt();
  if (rc != 0)
    return this->fail_("Ed25519 signature INVALID for downloaded image");

  md5_digest.calculate();
  char md5_hex[33] = {0};
  md5_digest.get_hex(md5_hex);
  md5_hex[32] = '\0';
  this->verified_md5_ = md5_hex;

  ESP_LOGI(TAG, "Ed25519 signature VALID over %u bytes (md5 %s)", (unsigned) bytes_read, md5_hex);
  return true;
}

}  // namespace ota_signature
}  // namespace esphome
