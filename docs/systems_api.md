# Elysia ECS System Signatures Guide (Systems API)

Elysia ECS features an immensely powerful type deduction system, supporting various function signatures. The scheduler automatically resolves dependencies and injects resources based on the signature.

---

## 1. Component Query Systems

The most commonly used systems, for processing entities with specific component combinations.

### A. Auto-Query (Lambda Each)
The scheduler automatically constructs a `Query` based on the parameter types.
```cpp
// Signature: void(Component&, const Component&, ...)
// Best for: Simple entity logic updates
app.system("Move").run([](Pos& p, const Vel& v) {
    p.x += v.dx;
    p.y += v.dy;
}).build();
```

### B. With CommandBuffer Injection
When you need to add/remove components or destroy entities, the first parameter must be `CommandBuffer&`.
```cpp
// Signature: void(CommandBuffer&, Component&, ...)
// Best for: Trigger-based logic, like collision destruction or state transitions
app.system("Expire").run([](CommandBuffer& cmd, Entity e, const LifeTime& lt) {
    if (lt.value <= 0) cmd.despawn(e);
}).build();
```

---

## 2. Global Resource Systems

Used for logic that depends on global resources rather than specific entities.

### A. Resource Injection (Res<T>)
Use the `Res<T>` wrapper to declare a dependency on a global resource.
```cpp
// Signature: void(Res<T>, Res<U>, ...)
// Best for: Environmental logic, like applying gravity, global timers, input handling
app.system("Gravity").run([](Res<Gravity> g, Res<DeltaT> dt) {
    // The scheduler automatically checks if the resource exists, asserting failure if not
    g->apply(dt->value);
}).build();
```

### B. Mixed Mode (Query + Res)
Resource parameters can be mixed with component parameters (only in auto-deduction mode).
```cpp
// 🌸 Advanced Usage
app.system("WindEffect").run([](Pos& p, Res<WindRes> wind) {
    p.x += wind->force;
}).build();
```

---

## 3. High-Performance Bulk Systems

Designed to squeeze out CPU cache performance by directly manipulating memory blocks.

### A. Physical Chunk Iteration (BulkSystem)
The first parameter is `size_t` (Count) or `int` (Count), followed by pointers.
```cpp
// Signature: void(int count, Component* p, const Component* v, ...)
// Best for: Physics engines, render batching, massive particle systems
// Performance: 2-5x faster than lambda-each, with no lambda wrapper overhead
void compute_physics(int n, Pos* p, const Vel* v) {
    for(int i=0; i<n; ++i) {
        p[i].x += v[i].dx;
    }
}

// Registration
app.system("Physics").run(compute_physics).build();
```

---

## 4. Raw Access Systems (World Systems)

The most flexible systems, possessing complete control over the World.

### A. Raw Pointer Access
```cpp
// Signature: void(World*)
// Best for: Complex genesis logic requiring dynamic Query building or manual Registry manipulation
app.system("Genesis").run([](World* w) {
    auto& res = w->resources();
    if(!res.get<MapData>()) res.add(MapData::load("level1.map"));
}).build();
```

---

## 5. Stateful Systems (Functor Systems)

Used when a system needs to hold its own state (like caching a Query).

```cpp
struct MySystem {
    Query<Pos, const Vel> q; // Cache Query for performance

    // init hook: called right before the system runs for the first time
    void init(World* w) { w->update_query(q); }

    // Core logic
    void operator()(World* w) {
        for(auto cv : q.chunks()) {
            cv.iter([](size_t n, Pos* p, const Vel* v) {
                // Execute high-performance iteration
            });
        }
    }
};

// Registration: via the scheduler's SystemBuilder
app.scheduler().system("MySystem").run<MySystem>().build();
```

---

## 6. Startup Systems

The core library provides an independent startup scheduler that runs exactly once before the main loop.

```cpp
app.add_startup_system("Init", [](World& w) {
    w.resources().add(GlobalConfig{.gravity = 9.8f});
});
```

---

## 💡 Developer Advice (Optimization)

1.  **Prefer BulkSystems**: For high-frequency tasks involving over 10,000 entities, Bulk pointer iteration is the only choice.
2.  **Avoid Frequent Syncs**: Every `CommandBuffer` submission fractures a wave. Try to handle them uniformly at the end of a Stage.
3.  **Res<T> vs World***: If you only need to access a few specific resources, prioritize `Res<T>`. It allows the scheduler to discover and parallelize your task earlier.

## World runtimes

A `Scheduler` stores system definitions and dependencies. Instantiate a separate
runtime for each world; every runtime owns its system instances, query caches,
command buffers, and initialization state.

```cpp
Scheduler scheduler;
scheduler.system("Move").run<MovementSystem>().build();
auto runtimeA = scheduler.instantiate(worldA);
auto runtimeB = scheduler.instantiate(worldB);
auto executorA = SerialExecutor::build_from(runtimeA);
auto executorB = SerialExecutor::build_from(runtimeB);
// These calls may run on separate threads.
executorA->run(&worldA);
executorB->run(&worldB);
```

Taskflow and ForkUnion accept the same runtime handles. Passing an existing
runtime to another executor preserves its state; use those executors sequentially.
`build_from(scheduler)` instead creates a fresh snapshot and binds it on first run.
A runtime rejects a different world. Its world must outlive it.

Runtime creation snapshots the definitions, so subsequent scheduler edits affect
only new runtimes. The scheduler may be destroyed while its runtimes remain alive.
Serialize snapshot creation with other snapshot creation and scheduler edits.
Execution of already-created runtimes does not access the scheduler.

`run<T>()` constructs and initializes a fresh functor per runtime. `run(callable)`
transfers the callable into the definition once; each runtime copies that unexecuted
prototype and creates fresh adapter query state. Callables must remain copyable.
References, raw pointers, and `shared_ptr` captures retain their normal sharing
semantics; callers must synchronize shared external state. Already-prepared query
objects captured by user code are also copied as supplied; prefer `run<T>()` for
systems owning world-specific query state.

`App` retains one runtime across executor changes. Register systems before its
first initialization; later definition edits do not modify that runtime.

## System exceptions

Serial, Taskflow, and ForkUnion propagate thrown system exceptions from `run()`
to its caller, preserving their type and payload. On failure, dependent work is
skipped and already-running workers finish before the exception reaches the caller.
With simultaneous worker failures, one original exception is propagated; which one
is unspecified. Independent work may already have run.

Before rethrowing, the runtime discards pending commands in its injected system
command buffers and releases their payloads. Earlier world mutations, submitted
commands, and published messages are not rolled back. Application-owned queues and
world command buffers are not managed by this cleanup. Command payload destructors
must not throw.

There is no automatic repair or retry. A caller may explicitly repair valid world
state and start another run, or discard the world/runtime. Failures inside component
lifecycle operations may leave application or world state unsuitable for reuse;
propagating an exception is not a guarantee of transactional safety. Expected
application outcomes can be communicated through messages instead of throwing.

## Taskflow exclusivity

Explicit exclusive systems, systems taking `World*`, and `ApplyDeferred` execute
alone within their runtime. Taskflow keeps one graph, with parallel sections
separated by these systems. Each section retains its original dependencies;
ordinary topological layers do not introduce synchronization barriers. Unrelated
work is placed before or after an exclusive system using a valid topological
order; use explicit dependencies when that placement matters. Cyclic schedules
are rejected during Taskflow executor construction.

Exclusivity is local to a runtime: independent world runtimes can still execute
concurrently. Ordinary systems must declare ordering needed for conflicting
access; this does not introduce automatic component conflict detection.
