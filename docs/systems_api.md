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
