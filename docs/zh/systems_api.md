# Elysia ECS 系统签名指南 (Systems API)

Elysia ECS 拥有极其强大的类型推导系统，支持多种函数签名。调度器会自动根据签名解析依赖并注入资源。

---

## 1. 组件查询系统 (Query Systems)

最常用的系统，用于处理特定组件组合的实体。

### A. 自动查询 (Lambda Each)
调度器根据参数类型自动构建 `Query`。
```cpp
// 签名：void(Component&, const Component&, ...)
// 适用：简单的实体逻辑更新
app.system("Move").run([](Pos& p, const Vel& v) {
    p.x += v.dx;
    p.y += v.dy;
}).build();
```

### B. 带有命令缓冲 (CommandBuffer Injection)
当需要增加/移除组件或销毁实体时，第一个参数必须是 `CommandBuffer&`。
```cpp
// 签名：void(CommandBuffer&, Component&, ...)
// 适用：触发式逻辑，如碰撞销毁、状态转换
app.system("Expire").run([](CommandBuffer& cmd, Entity e, const LifeTime& lt) {
    if (lt.value <= 0) cmd.despawn(e);
}).build();
```

---

## 2. 全局资源系统 (Global Systems)

用于处理不依赖于特定实体，而是依赖全局资源的逻辑。

### A. 资源注入 (Res<T>)
使用 `Res<T>` 包装器来声明对全局资源的依赖。
```cpp
// 签名：void(Res<T>, Res<U>, ...)
// 适用：环境逻辑，如重力应用、全局计时、输入处理
app.system("Gravity").run([](Res<Gravity> g, Res<DeltaT> dt) {
    // 调度器会自动检查资源是否存在，不存在则断言失败
    g->apply(dt->value);
}).build();
```

### B. 混合模式 (Query + Res)
资源参数可以与组件参数混合（仅限于自动推导模式）。
```cpp
// 🌸 高级用法
app.system("WindEffect").run([](Pos& p, Res<WindRes> wind) {
    p.x += wind->force;
}).build();
```

---

## 3. 高性能批量系统 (Bulk Systems)

为了榨干 CPU 缓存性能，直接操作内存块。

### A. 物理 Chunk 迭代 (BulkSystem)
第一个参数为 `size_t` (Count) 或 `int` (Count)，后续为指针。
```cpp
// 签名：void(int count, Component* p, const Component* v, ...)
// 适用：物理引擎、渲染批处理、大规模粒子系统
// 性能：比 lambda-each 快 2-5 倍，无 lambda 包装开销
void compute_physics(int n, Pos* p, const Vel* v) {
    for(int i=0; i<n; ++i) {
        p[i].x += v[i].dx;
    }
}

// 注册方式
app.system("Physics").run(compute_physics).build();
```

---

## 4. 原始访问系统 (World Systems)

最灵活的系统，拥有对 World 的完全控制权。

### A. 原始指针访问
```cpp
// 签名：void(World*)
// 适用：需要动态构建 Query、手动操作 Registry 的复杂创世逻辑
app.system("Genesis").run([](World* w) {
    auto& res = w->resources();
    if(!res.get<MapData>()) res.add(MapData::load("level1.map"));
}).build();
```

---

## 5. 状态化系统 (Functor Systems)

当系统需要持有自己的状态（如 Query 缓存）时使用。

```cpp
struct MySystem {
    Query<Pos, const Vel> q; // 缓存 Query 提高性能

    // init 钩子：在系统第一次运行前被调用
    void init(World* w) { w->update_query(q); }

    // 核心逻辑
    void operator()(World* w) {
        for(auto cv : q.chunks()) {
            cv.iter([](size_t n, Pos* p, const Vel* v) {
                // 执行高性能迭代
            });
        }
    }
};

// 注册方式：通过 scheduler 获取 SystemBuilder
app.scheduler().system("MySystem").run<MySystem>().build();
```

---

## 6. Startup 系统

核心库提供独立的 startup 调度器，在 main 循环之前运行一次。

```cpp
app.add_startup_system("Init", [](World& w) {
    w.resources().add(GlobalConfig{.gravity = 9.8f});
});
```

---

## 💡 开发者建议 (Optimization)

1.  **优先使用 BulkSystem**：对于实体数量超过 10,000 的高频任务，Bulk 指针迭代是唯一选择。
2.  **避免频繁 Sync**：每次 `CommandBuffer` 的提交都会造成一次波次断裂。尽量在 Stage 的末尾统一处理。
3.  **Res<T> vs World***：如果只需访问几个固定资源，优先使用 `Res<T>`，它能让调度器更早地发现并并行化你的任务。
