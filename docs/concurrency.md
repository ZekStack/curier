# Concurrency and lifecycle

## Ownership model

Curier has one worker task and one bounded ring queue. Public `send()` calls
validate the subscription and payload before the operation becomes visible to
the worker. Accepted subscription fields and serialized payload data are copied
into storage using `CurierConfig::memory.allocation`.

The configured `queueSize` counts all accepted work:

```text
accepted = queued + active
```

When that bound is reached, `send()` returns `QueueFull` immediately and no
callback is retained.

Curier owns orchestration and lifecycle policy. Strata owns the placement and
lifetime of Curier-created memory plus the worker task stack/TCB, recursive
mutex control storage, and shutdown semaphore control storage.

## Callback context

Terminal callbacks run serially on Curier's worker task and are never invoked
while the runtime mutex is held. A callback may enqueue another send if the
queue has capacity.

Callbacks must not call `end()` or destroy their Curier instance. `end()`
detects the worker context and returns `Busy`. Destroying the instance from a
callback is a fatal programming error because the callback is executing inside
a member function owned by that instance. Schedule destruction on another
application task.

## Exactly-once completion

Every successfully accepted send has one terminal path:

* success after a 2xx response;
* terminal clock, crypto, transport, or HTTP failure;
* application retry-policy termination;
* cancellation during shutdown.

The callback is released only after invocation. Diagnostic completion counters
are updated after the callback returns.

## Shutdown

`end()` serializes lifecycle changes, publishes `Stopping`, wakes the worker,
and waits on a `Strata::FreeRTOS::BinarySemaphore`. It does not depend on free
queue capacity.

The active ESP-IDF HTTP request is not forcibly destroyed from another task.
Its configured request timeout bounds the normal wait. If the public shutdown
timeout expires first, `end()` returns `Timeout` and preserves all worker-owned
state. A later `end()` call can finish cleanup.

Queued work is cancelled by the worker. Retry waits use task notifications and
wake immediately for shutdown.

After all jobs and callbacks are complete, the worker publishes final stack
diagnostics, gives the shutdown semaphore, and suspends without touching
runtime state again. `end()` then calls `reset()` on the owned
`Strata::FreeRTOS::Task` from the owner context. Strata deletes the FreeRTOS task
and releases the placed stack and internal task control block.

Only after task reset does Curier release the queue, shutdown semaphore, JWT
cache, cryptographic state, and copied runtime configuration. A successful
`end()` therefore guarantees that the worker and all Curier-owned runtime
storage have been released before the function returns. Immediate
reinitialization is supported.

## Time-provider replacement

Provider storage and VAPID cache generation are protected by the runtime mutex.
The worker copies the provider before calling it, so application code is not
executed under Curier's lock. Changing the provider invalidates all cached JWTs.
