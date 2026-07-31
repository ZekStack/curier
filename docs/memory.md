# Memory model

Curier bounds accepted work by `queueSize`. Each accepted job owns:

* one copied endpoint, `p256dh` key, and `auth` secret;
* one serialized plaintext payload bounded by `maxPayloadBytes`;
* one copied `std::function` callback;
* queue-record and standard-library allocation overhead.

The active worker additionally holds an encrypted body, ephemeral key material,
an HTTP client, and a VAPID JWT. Curier caches at most four JWTs by push-service
origin.

Queue records use normal heap storage. Worker stacks use the selected
`CurierStackType`; `Auto` prefers a PSRAM stack where capability-aware ESP-IDF
task creation is available and falls back to internal RAM.

Standard-library strings, vectors, JSON documents, and callbacks can allocate.
Curier uses checked non-throwing allocation for its implementation, queue, and
job records, but toolchain standard-library allocation behavior still applies.
User callbacks and time/retry providers must not throw exceptions.

The VAPID private-key copy, cached JWTs, subscription auth secrets, accepted
plaintext payload buffers, deterministic crypto-test inputs, and intermediate
key buffers are overwritten before normal release. This reduces secret residue
but does not replace whole-device heap, crash-dump, and physical security
controls.

A successful `end()` deletes the worker from the owner task before freeing the
queue, semaphore, cryptographic state, and configuration. Capability-created
PSRAM tasks are deleted with `vTaskDeleteWithCaps`; normal tasks are deleted
with `vTaskDelete`. This makes task stack and TCB reclamation part of the public
shutdown completion boundary rather than deferred worker self-cleanup.

`tests/esp32/LifecycleSmoke` repeatedly initializes and ends Curier with
Internal, Auto, and PSRAM stack selection where available, then compares
internal and external heap against a warmed baseline. Run this test on every
production board because compile success cannot qualify allocator recovery.

Use `CurierDiagnostics` to measure queue and stack pressure on each real target.
Compile success does not qualify a stack size for live TLS, cryptography,
payload, retry, and callback workloads.
