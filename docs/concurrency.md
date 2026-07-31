# Concurrency and lifecycle

## Ownership model

Curier has one worker task and one bounded ring queue. Public `send()` calls
validate and deep-copy the subscription, serialized payload, and callback
before the operation becomes visible to the worker.

The configured `queueSize` counts all accepted work:

```txt
accepted = queued + active
```

When that bound is reached, `send()` returns `QueueFull` immediately and no
callback is retained.

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
and waits on a dedicated worker-ready semaphore. It does not depend on free
queue capacity.

The active ESP-IDF HTTP request is not forcibly destroyed from another task.
Its configured request timeout bounds the normal wait. If the public shutdown
timeout expires first, `end()` returns `Timeout` and preserves all worker-owned
state. A later `end()` call can finish cleanup.

Queued work is cancelled by the worker. Retry waits use task notifications and
wake immediately for shutdown.

After all jobs and callbacks are complete, the worker publishes final stack
diagnostics and suspends without touching runtime state again. `end()` then
deletes the suspended task from the owner context using the same normal or
capability-aware API used for its allocation. Only after task deletion does it
free the queue, semaphore, cryptographic state, JWT cache, and configuration.

A successful `end()` therefore guarantees that the worker task and its owned
runtime allocations have been released before the function returns. Immediate
reinitialization is supported.

## Time-provider replacement

Provider storage and VAPID cache generation are protected by the runtime mutex.
The worker copies the provider before calling it, so application code is not
executed under Curier's lock. Changing the provider invalidates all cached JWTs.
