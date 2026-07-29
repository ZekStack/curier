# Changelog

All notable changes to Curier are documented in this file.

## 0.1.0

- Add asynchronous Web Push delivery over RFC 8291 `aes128gcm`.
- Add VAPID ES256 authentication with bounded, per-origin JWT caching.
- Add an owned FreeRTOS worker, bounded queue, deterministic shutdown, and diagnostics.
- Add typed and ArduinoJson v7 payload overloads.
- Add standard system-time use with a replaceable time-provider callback.
- Add configurable fixed, exponential, and application-defined retry policies.
