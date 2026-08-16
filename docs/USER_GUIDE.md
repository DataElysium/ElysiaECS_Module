# Elysia ECS Developer Guide (v1.0)

## 1. System Declaration: Lambda vs Functor

### ✅ Option A: Rapid Lambda (Auto-Update)
Suitable for simple logic. `world.query<T>()` returns a temporary view that automatically synchronizes with the current world state.
```cpp
app.system("Move").run([](World* w) {
    w->query<Pos, Vel>().each([](Entity e, Pos& p, Vel& v) {
        p.x += v.dx;
    });
});
```

### ✅ Option B: Persistent Functor (Manual Update)
Suitable for complex logic. If `Query` is stored as a system member to optimize performance, **you must manually call `update_query`**.
```cpp
struct MovementSystem {
    Query<Pos, Vel> q; // Stored as a member

    void operator()(World* w) {
        // ⚠️ Critical: Query does not automatically detect structural changes in World
        w->update_query(q); 
        
        q.each([](Entity e, Pos& p, Vel& v) {
            p.x += v.dx;
        });
    }
};
```

---

## 2. The Two Forms of CommandBuffer

### 🚀 Mode 1: Stateless (Lightweight)
If the system is only responsible for modifying data on **existing entities** (`insert`, `remove`, `despawn`), you do not need to bind an Index.
```cpp
CommandBuffer cmd; // Default construction
cmd.insert(existing_e, Tag{});
```

### ☢️ Mode 2: With Allocator (Spawn Mode)
If you need to **create new entities** during parallel execution phases, you must reserve IDs via `EntityIndex`.
```cpp
// Bind during system initialization or in a Factory
CommandBuffer cmd(&world.index());
Entity new_e = cmd.spawn(); // Atomically reserves an ID at this moment
```

---

## 3. Performance Guidelines: POD and Physical Mirroring
- **Tier 0 Acceleration (`RawArchive`)**: Strictly limited to `std::is_trivially_copyable` components.
- **Patching Mechanism**: Utilizing the automatic fusion capability of `CommandBuffer`, it supports merging data for the same entity from multiple `.raw` or `.csv` files.
