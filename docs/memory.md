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

The VAPID private-key copy, cached JWTs, subscription auth secrets, and accepted
plaintext payload buffers are overwritten before normal release. This reduces
secret residue but does not replace whole-device heap, crash-dump, and physical
security controls.

Use `CurierDiagnostics` to measure queue and stack pressure on each real target.
Compile success does not qualify a stack size for live TLS, cryptography,
payload, retry, and callback workloads.
