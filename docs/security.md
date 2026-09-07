# Security

## Secret handling

The VAPID private key authorizes sends for the application server identity.
Provision it as a secret, avoid logging it, and do not commit production values.

Curier validates the public/private relationship during initialization.
Temporary private-key, ECDH, HKDF, AES, nonce, and signature buffers are
explicitly zeroed before release. Curier also clears copied VAPID private-key
storage, cached JWTs, accepted plaintext payloads, and copied subscription auth
secrets during their normal release paths.

Curier `v0.2.0` routes those Curier-owned buffers through
`CurierConfig::memory.allocation`. If external placement is selected, sensitive
runtime buffers may therefore reside in external RAM. The worker stack follows
`memory.taskStack` and can also contain ephemeral cryptographic locals. Secure
clearing remains active regardless of placement.

Caller-created `std::string`, `JsonDocument`, and callback/provider captures are
outside Curier's allocator boundary, as are allocations performed internally by
Mbed TLS and ESP-IDF. Whole-device heap, PSRAM, crash dumps, and physical access
remain part of the device trust boundary.

The deterministic encryption test accepts explicit ephemeral inputs only through
an internal API. Production sends still generate a fresh sender key and salt
from the Mbed TLS DRBG.

## Subscription validation

Subscription endpoints and keys arrive from an external browser boundary.
Curier requires:

* an HTTPS endpoint within `maxEndpointBytes`;
* no URL user information;
* a valid DNS, IPv4, or bracketed IPv6 host and optional port;
* no fragments, backslashes, malformed brackets, or ambiguous port separators;
* a 65-byte uncompressed P-256 `p256dh` key;
* a 16-byte `auth` secret;
* valid base64url input.

The endpoint origin used for VAPID is built only after this validation. This
prevents the HTTP client and VAPID signer from interpreting malformed
authorities differently.

This validation does not establish whether the endpoint is still subscribed.
HTTP 404 or 410 should cause the application to retire the stored subscription.

## Cryptographic qualification

The host suite injects the published RFC 8291 Appendix A sender private key and
salt into Curier's production encryption path and checks the complete encrypted
body byte-for-byte. VAPID tests decode the generated claims and independently
verify the raw ES256 signature with the configured public key.

These tests protect the wire format and derivation logic, but they do not replace
Mbed TLS maintenance, secure provisioning, or production-device validation.

## TLS

Certificate and host-name verification are enabled by default when the ESP-IDF
certificate bundle is available. A custom CA or a prepared global CA store can
be selected instead.

Avoid `skipTlsCommonNameCheck` in production. It allows a valid certificate for
the wrong host to authenticate the connection.

## Clock

VAPID depends on trustworthy wall-clock time. The custom provider is a trust
boundary: Curier accepts its Unix timestamp and signs JWTs from it. A clock
rollback does not reuse a JWT after provider replacement because that operation
clears the cache.

## Logging

Curier result messages do not include endpoints, subscription keys, payload
contents, VAPID tokens, or private keys. Applications should preserve that
boundary in their own logs.
