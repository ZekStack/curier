# Memory model

Curier `v0.2.0` uses Strata `v0.1.2` as its memory-placement and low-level
FreeRTOS ownership layer.

## Policy

```cpp
CurierConfig config;
config.memory.allocation = Strata::Placement::Default;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

`memory.allocation` applies to Curier-owned movable runtime storage. This
includes:

* queue backing and job records;
* accepted endpoint, `p256dh`, and `auth` copies;
* serialized plaintext payloads;
* copied runtime configuration strings;
* VAPID JWT cache entries;
* Curier-created ArduinoJson documents;
* crypto decode, HKDF, ciphertext, Base64, and JWT working buffers;
* HTTP authorization and response scratch storage that requires dynamic memory.

`memory.taskStack` controls only the Curier worker stack. `PreferExternal`
preserves the old automatic PSRAM behavior by falling back to internal memory.
`RequireExternal` is strict and fails task creation when external storage cannot
be provided.

Safety-critical FreeRTOS control storage remains internal: Strata keeps task
TCBs, recursive-mutex control blocks, and binary-semaphore control blocks in
internal RAM even when movable storage prefers external memory.

## Caller-owned and dependency-owned memory

Public `CurierConfig`, `CurierSubscription`, `CurierPayload`, and caller-created
`JsonDocument` objects remain normal caller-owned C++ objects. Allocations made
while the application constructs those objects occur before Curier accepts
them and are outside Curier's memory policy.

`std::function` remains the callback/time-provider/retry-policy surface. Memory
allocated by the caller while constructing captures is likewise outside
Curier's owned allocation boundary; Curier moves or copies the resulting
function object as part of its API contract.

ESP-IDF HTTP client and Mbed TLS internals may allocate memory inside those
dependencies. Curier does not intercept their allocators. The Strata policy
covers allocations directly owned or explicitly created by Curier.

## Queue bound

`queueSize` bounds all accepted work, including the active job. Each accepted
job owns its copied subscription, serialized plaintext payload, callback, and
job-record overhead. The active worker additionally owns an encrypted body,
ephemeral cryptographic working storage, HTTP request scratch data, and a VAPID
JWT. Curier caches at most four JWTs by push-service origin.

## Secret cleanup

The copied VAPID private key, cached JWTs, subscription auth secrets, accepted
plaintext payloads, deterministic crypto-test inputs, and intermediate key
buffers are explicitly overwritten before normal release where Curier owns the
storage. Selecting external placement does not disable this cleanup; sensitive
Curier-owned buffers may reside in external RAM when that policy is requested.

This reduces secret residue but does not replace whole-device heap, crash-dump,
and physical security controls.

## Shutdown ownership

The worker never self-deletes its Strata task owner. When shutdown completes its
work, it publishes final diagnostics, gives a Strata binary semaphore, and
suspends. `end()` then resets `Strata::FreeRTOS::Task` from the calling owner
context. That reset deletes the FreeRTOS task and releases its stack and TCB
before queue, semaphore, cryptographic state, JWT cache, and runtime config are
released.

A successful `end()` therefore includes physical task-storage reclamation in
the public completion boundary. A timeout preserves the `Stopping` state and
owned storage so a later `end()` can finish safely.

## Diagnostics and qualification

`CurierDiagnostics` separates policy from observed storage. In particular,
`requestedStackPlacement` can be `PreferExternal` while `stackRegion` is
`Internal` after a valid fallback. Queue storage exposes the same distinction
through `queueStoragePlacement` and `queueStorageRegion`.

`tests/esp32/LifecycleSmoke` repeatedly initializes and ends Curier with
Internal, PreferExternal, and RequireExternal stack policies where supported,
then compares internal and external heap against a warmed baseline. Run this
test on every production board because compile success does not qualify
allocator recovery or a safe stack size.
