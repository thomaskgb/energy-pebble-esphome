#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "esphome/components/http_request/http_request.h"

namespace esphome {
namespace ota_signature {

/// Verifies a detached Ed25519 signature over a firmware image fetched by
/// streaming it over HTTP(S), without buffering the image in RAM.
///
/// Fail-closed contract: verify_url() returns true ONLY when the image was
/// fully downloaded and its Ed25519 signature checks out against the embedded
/// public key. Every other outcome (missing/malformed signature, HTTP error,
/// short read, unconfigured key, bad signature) returns false.
///
/// On success, verified_md5() holds the MD5 (lowercase hex) of the exact bytes
/// that were verified. Passing that to ota.http_request.flash pins the second
/// (flashing) download to the verified content, so a server that swaps the
/// image between the two downloads is detected (MD5 second-preimage of a fixed
/// signed image is not a practical attack).
class OtaSignatureVerifier : public Component {
 public:
  void set_http_request(http_request::HttpRequestComponent *http) { this->http_ = http; }
  void set_public_key(std::array<uint8_t, 32> key) { this->public_key_ = key; }

  void dump_config() override;

  /// Stream `url` and verify `signature_b64` (base64 of the raw 64-byte
  /// Ed25519 signature) over the exact downloaded bytes.
  bool verify_url(const std::string &url, const std::string &signature_b64);

  /// MD5 (lowercase hex) of the bytes verified by the last successful
  /// verify_url() call; empty otherwise.
  const std::string &verified_md5() const { return this->verified_md5_; }

  /// Human-readable reason for the last verify_url() failure; empty on success.
  const std::string &last_error() const { return this->last_error_; }

 protected:
  bool fail_(const std::string &reason);

  http_request::HttpRequestComponent *http_{nullptr};
  std::array<uint8_t, 32> public_key_{};
  std::string verified_md5_;
  std::string last_error_;
};

}  // namespace ota_signature
}  // namespace esphome
