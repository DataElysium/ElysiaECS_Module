# Elysia ECS 开发者指南 (v1.0)

## 1. 系统声明：Lambda vs Functor

### ✅ 方案 A：快速 Lambda (自动更新)
适合简单逻辑。`world.query<T>()` 返回的是一个临时视图，会自动与当前世界同步。
```cpp
app.system("Move").run([](World* w) {
    w->query<Pos, Vel>().each([](Entity e, Pos& p, Vel& v) {
        p.x += v.dx;
    });
});
```

### ✅ 方案 B：持久化 Functor (手动更新)
适合复杂逻辑。如果 `Query` 作为系统成员存储以优化性能，**必须手动调用 `update_query`**。
```cpp
struct MovementSystem {
    Query<Pos, Vel> q; // 成员存储

    void operator()(World* w) {
        // ⚠️ 关键：Query 不会自动感知 World 的结构变化
        w->update_query(q); 
        
        q.each([](Entity e, Pos& p, Vel& v) {
            p.x += v.dx;
        });
    }
};
```

---

## 2. CommandBuffer 的两种形态

### 🚀 模式 1：无状态 (轻量级)
如果系统只负责给**已有实体**修改数据（`insert`, `remove`, `despawn`），不需要绑定 Index。
```cpp
CommandBuffer cmd; // 默认构造
cmd.insert(existing_e, Tag{});
```

### ☢️ 模式 2：带发号器 (Spawn 模式)
如果你需要在并行阶段**创建新实体**，必须通过 `EntityIndex` 预留 ID。
```cpp
// 在系统初始化或 Factory 中绑定
CommandBuffer cmd(&world.index());
Entity new_e = cmd.spawn(); // 此时才会去原子预留 ID
```

---

## 3. 性能准则：POD 与物理镜像
- **T0 级加速 (`RawArchive`)**：仅限 `std::is_trivially_copyable` 的组件。
- **Patching 机制**：利用 `CommandBuffer` 的自动融合能力，支持从多个 `.raw` 或 `.csv` 文件合并同一个实体的数据。
