# Changelog

All notable changes to Curier are documented in this file.

## 0.2.0

- Adopt the ZekStack-standard `Strata::MemoryPolicy` for Curier-owned allocations and worker task-stack placement.
- Replace `CurierStackType` with `Strata::Placement`, mapping automatic stacks to `PreferExternal`, internal stacks to `Internal`, and strict PSRAM stacks to `RequireExternal`.
- Route Curier implementation, queue backing, jobs, accepted subscriptions and payloads, JWT cache, crypto working storage, and HTTP scratch storage through Strata ownership and allocators.
- Replace Curier-specific recursive mutex, binary semaphore, task-stack, task-control-block, and capability-aware task allocation with Strata FreeRTOS owners.
- Preserve the existing owner-task shutdown handoff so successful `end()` still means the worker task, stack, TCB, queue, cryptographic state, and runtime configuration have been released.
- Use Strata's ArduinoJson allocator for Curier-created JSON documents while keeping caller-provided `JsonDocument` instances caller-owned.
- Report requested allocation/task placement separately from observed queue and task memory regions in diagnostics.
- Add source audits preventing direct Curier-owned heap, task, queue, and semaphore allocation paths from returning.
- Extend lifecycle qualification for Internal, PreferExternal, and RequireExternal placement policies across supported ESP32 targets.
- Require Strata `v0.1.2` and bump Curier metadata to `v0.2.0`.

## 0.1.0

- Add asynchronous Web Push delivery over RFC 8291 `aes128gcm`.
- Add VAPID ES256 authentication with bounded, per-origin JWT caching.
- Add an owned FreeRTOS worker, bounded queue, deterministic shutdown, and diagnostics.
- Delete worker tasks from the owner context before successful shutdown returns, including capability-created PSRAM tasks.
- Fail deterministically when a Curier instance is destroyed from its callback task instead of leaking the runtime.
- Harden endpoint parsing against malformed authorities, user information, backslashes, invalid DNS labels, and unbracketed IPv6.
- Add an RFC 8291 known-answer encryption test and independent VAPID JWT signature verification.
- Add deterministic transport-backed queue, callback, retry, cancellation, timeout-recovery, and lifecycle stress tests.
- Qualify examples and lifecycle-test compilation across ESP32, ESP32-S3, ESP32-C3, and ESP32-P4.
- Add typed and ArduinoJson v7 payload overloads.
- Add standard system-time use with a replaceable time-provider callback.
- Add configurable fixed, exponential, and application-defined retry policies.
