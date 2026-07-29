# Web Push protocol

Curier implements the modern standards used for encrypted Web Push:

* RFC 8291 Web Push message encryption;
* RFC 8188 encrypted content coding with `aes128gcm`;
* RFC 8292 VAPID authentication using ES256;
* HTTPS POST through ESP-IDF `esp_http_client`.

The legacy `aesgcm` content encoding is intentionally not implemented.

## Encrypted body

Each send generates a fresh P-256 sender key pair and 16-byte salt. Curier uses
the subscription's `p256dh` key and 16-byte `auth` secret to derive:

1. the RFC 8291 input key material;
2. the RFC 8188 content-encryption key;
3. the RFC 8188 nonce.

The plaintext JSON is terminated with the final-record delimiter and encrypted
with AES-128-GCM. Curier emits one record with record size 4096.

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

## VAPID audience and caching

The JWT audience is the normalized HTTPS origin of the subscription endpoint:
scheme, lowercase host, and a non-default port when present. Paths, query
strings, fragments, and user information are never part of the audience.

JWTs have a 12-hour lifetime and are refreshed five minutes before expiration.
Curier keeps at most four cached origins and invalidates them when the time
provider changes.
