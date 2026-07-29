# Security

## Secret handling

The VAPID private key authorizes sends for the application server identity.
Provision it as a secret, avoid logging it, and do not commit production values.

Curier validates the public/private relationship during initialization.
Temporary private-key, ECDH, HKDF, AES, nonce, and signature buffers are
explicitly zeroed before release. Standard-library capacity and allocator
behavior can still leave copies outside Curier's direct control, so the ESP32
heap and crash dumps remain part of the device trust boundary.

## Subscription validation

Subscription endpoints and keys arrive from an external browser boundary.
Curier requires:

* an HTTPS endpoint within `maxEndpointBytes`;
* no URL user information;
* a valid host and optional port;
* a 65-byte uncompressed P-256 `p256dh` key;
* a 16-byte `auth` secret;
* valid base64url input.

This validation does not establish whether the endpoint is still subscribed.
HTTP 404 or 410 should cause the application to retire the stored subscription.

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
