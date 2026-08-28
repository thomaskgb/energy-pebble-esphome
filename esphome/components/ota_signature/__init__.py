"""Ed25519 firmware signature verification for HTTP OTA updates.

Streams the firmware image from its download URL, verifies a detached
Ed25519 signature (as served by /api/ota/check) against the public key
embedded at build time, and exposes the MD5 of the *verified* bytes so
the subsequent ota.http_request.flash download is pinned to the exact
content that passed verification.
"""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import http_request
from esphome.const import CONF_ID
from esphome.core import HexInt

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@thomaskgb"]
DEPENDENCIES = ["http_request"]
AUTO_LOAD = ["md5"]

CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_PUBLIC_KEY = "public_key"

# Documented placeholder — a device built with this key refuses every update
# (fail closed). Replace with the real key from `firmware_signing.py keygen`.
PLACEHOLDER_KEY = "0" * 64

ota_signature_ns = cg.esphome_ns.namespace("ota_signature")
OtaSignatureVerifier = ota_signature_ns.class_("OtaSignatureVerifier", cg.Component)


def _validate_public_key(value):
    value = cv.string_strict(value)
    if len(value) != 64 or any(c not in "0123456789abcdefABCDEF" for c in value):
        raise cv.Invalid(
            "public_key must be the Ed25519 public key as 64 hex characters "
            "(32 raw bytes), as printed by `firmware_signing.py keygen`"
        )
    return value.lower()


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(OtaSignatureVerifier),
        cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(
            http_request.HttpRequestComponent
        ),
        cv.Required(CONF_PUBLIC_KEY): _validate_public_key,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    key = config[CONF_PUBLIC_KEY]
    if key == PLACEHOLDER_KEY:
        _LOGGER.warning(
            "ota_signature: public_key is the all-zero PLACEHOLDER. This build "
            "will REFUSE every OTA update. Replace firmware_signing_pubkey with "
            "the real public key from `firmware_signing.py keygen` before "
            "releasing."
        )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    http_ = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    cg.add(var.set_http_request(http_))

    key_bytes = [HexInt(int(key[i : i + 2], 16)) for i in range(0, 64, 2)]
    cg.add(var.set_public_key(cg.ArrayInitializer(*key_bytes)))
