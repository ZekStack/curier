# Changelog

All notable changes to Curier are documented in this file.

## 0.1.0

- Add asynchronous Web Push delivery over RFC 8291 `aes128gcm`.
- Add VAPID ES256 authentication with bounded, per-origin JWT caching.
- Add an owned FreeRTOS worker, bounded queue, deterministic shutdown, and diagnostics.
- Delete worker tasks from the owner context before successful shutdown returns, including capability-created PSRAM tasks.
- Fail deterministically when a Curier instance is destroyed from its callback task instead of leaking the runtime.
- Harden endpoint parsing against malformed authorities, user information, backslashes, invalid DNS labels, and unbracketed IPv6.
- Add an RFC 8291 known-answer encryption test and independent VAPID JWT signature verification.
- Add deterministic transport-backed queue, callback, retry, cancellation, timeout-recovery, and lifecycle stress tests.
- Add typed and ArduinoJson v7 payload overloads.
- Add standard system-time use with a replaceable time-provider callback.
- Add configurable fixed, exponential, and application-defined retry policies.
