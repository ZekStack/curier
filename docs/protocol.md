# Web Push protocol

Curier implements the modern standards used for encrypted Web Push:

* RFC 8291 Web Push message encryption;
* RFC 8188 encrypted content coding with `aes128gcm`;
* RFC 8292 VAPID authentication using ES256;
* HTTPS POST through ESP-IDF `esp_http_client`.

The legacy `aesgcm` content encoding is intentionally not implemented.

## Encrypted body

Each production send generates a fresh P-256 sender key pair and 16-byte salt.
Curier uses the subscription's `p256dh` key and 16-byte `auth` secret to derive:

1. the RFC 8291 input key material;
2. the RFC 8188 content-encryption key;
3. the RFC 8188 nonce.

The plaintext JSON is terminated with the final-record delimiter and encrypted
with AES-128-GCM. Curier emits one record with record size 4096.

The host crypto test injects the sender private key and salt published in RFC
8291 Appendix A into the same encryption path used by production. It compares
the complete 145-byte encrypted body byte-for-byte with the RFC result.

## HTTP request

Curier sends:

```txt
POST <subscription endpoint>
Authorization: vapid t=<JWT>, k=<public key>
Content-Encoding: aes128gcm
Content-Type: application/octet-stream
TTL: <configured seconds>
```

Any HTTP 2xx response is considered accepted. Other HTTP responses are exposed
as `HttpError` with their status code.

The HTTP transport has an internal compile-time test seam. With
`CURIER_ENABLE_TEST_HOOKS`, target tests can inject deterministic responses and
blocking operations without contacting a push service. The public Curier API is
unchanged.

## VAPID audience and caching

The JWT audience is the normalized HTTPS origin of the subscription endpoint:
scheme, lowercase host, and a non-default port when present. Paths, query
strings, fragments, and user information are never part of the audience.
Malformed authorities, unbracketed IPv6, multiple port separators, backslashes,
and invalid DNS labels are rejected before queueing.

JWTs have a 12-hour lifetime and are refreshed five minutes before expiration.
Curier keeps at most four cached origins and invalidates them when the time
provider changes.

Host tests decode generated JWT claims and independently verify the raw ES256
`r || s` signature with the configured public key.
