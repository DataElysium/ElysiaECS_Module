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

## Schedule visualization

Export registered systems, set boundaries, and declared ordering to Mermaid:

```cpp
auto& meta = app.scheduler().meta_world();
auto dag = elysia::schedule::build_dag(meta);
auto text = elysia::schedule::to_mermaid(dag, meta);
// Convenience overload: to_mermaid(meta, MermaidDirection::LeftToRight)
```

The returned string contains a Mermaid flowchart, without Markdown fences. It
does not execute systems or initialize a world runtime. Names are escaped using
[Mermaid entity codes](https://mermaid.js.org/syntax/flowchart.html#entity-codes-to-escape-characters).
Exclusive systems and deferred flushes are annotated; set start/end nodes remain
explicit. The export shows declared ordering, not executor-added exclusive
barriers, actual worker placement, or a timing trace. Cyclic graphs can also be
exported for inspection.

An ordinary empty callable (`.run([] {})`) is a valid design-time system. There
is no special placeholder registration or execution restriction. Referenced names
with no system or set registration appear as `[unresolved]` nodes with their
original edges. Export does not validate or prune these slots.

## Compile options and empty slots

```cpp
using namespace elysia;
schedule::CompileOptions options;
options.missing_labels = schedule::MissingLabelPolicy::Fill; // default
options.prune_empty_endpoints = true;                       // default

auto executor = TaskflowExecutor::build_from(scheduler, options);
// SerialExecutor and ForkUnionExecutor accept the same options, including
// when building from an existing shared ScheduleRuntime.
```

`Fill` retains unresolved slots as dependency-only nodes. In particular,
`A -> Missing -> B` still orders A before B. This replaces the old behavior of
silently dropping edges to unknown names. `Reject` reports the unresolved names
and refuses compilation, even if the missing slots could have been pruned.
Validation occurs on the completed runtime snapshot, after plugin registration.
All executors reject dependency cycles, including cycles through empty slots.

Compilation then recursively prunes structural no-op sources and sinks (unresolved
slots and set boundaries). Interior empty joins remain, preserving ordering
without expanding fan-in/fan-out edges. Registered callables, including empty
lambdas, are not inspected or pruned. Set `prune_empty_endpoints = false` to retain
all structural nodes in the compiled DAG.

```cpp
auto logical = schedule::build_dag(scheduler.meta_world());
auto plan = schedule::compile_dag(scheduler.meta_world(), options);
auto logical_mermaid = schedule::to_mermaid(logical, scheduler.meta_world());
auto plan_mermaid = schedule::to_mermaid(plan, scheduler.meta_world());
```

Neither operation modifies the metaworld. An executor owns its runtime snapshot:
registering a previously missing system afterward affects only a newly compiled
runtime. The old runtime keeps its original behavior. Rebuilding from the scheduler
creates fresh runtime system state; this is not live state migration. Changes to
an already running plan are not supported.
