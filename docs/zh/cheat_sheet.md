# Elysia ECS 极速 API 参考 (Cheat Sheet)

## 1. 查询 (Query)
- **临时查询**：`world.query<T>().each(...)` — 内部自动调用 `update_query`，适合一次性遍历。
- **成员查询**：Functor 系统中作为成员存储 Query，每帧手动调用 `w.update_query(member_q);`（⚠️ Functor 必做，避免重复匹配开销）。

## 2. 命令 (CommandBuffer)
- **修改组件**：`cmd.insert(e, Data{})` — 无需绑定 Index，仅对已有实体操作。
- **产生实体**：`cmd.spawn()` — 需绑定 EntityIndex：
  - 方式 1（推荐）：`CommandBuffer cmd(&world.index());`
  - 方式 2：`CommandBuffer cmd; cmd.set_index(&world.index());`

## 3. 存档 (Archive)
- **全量镜像**：`RawArchive::pack(world)` — 返回 `Result<std::vector<char>>`，二进制 blob，POD 专用，GB/s 级别。
  - 解压：`RawArchive::unpack<T...>(world, data)`。
- **结构化快照**：`AuroraArchive::create(world, reg, {Format::Csv})` — 从 `SnapshotRegistry` 构建内存快照（`WorldArchive`），支持 `Columnar` / `Csv` / `Raw` 格式。
  - 加载：`load_aurora(world, cmd, reg, archive)`。

## 4. 调度 (Scheduler)
- **拓扑排序**：利用 `before()` / `after()` 建立系统依赖，执行器自动进行 DAG 排序。
- **并行安全**：在系统 Lambda 中获取 `CommandBuffer&` 是最安全的并行修改方式（deferred 操作，帧末统一 apply）。
- **Functor 系统**：`app.system("Name").run(MySystem{...}).build();` — 系统作为可调用对象，支持状态持久化。

## 5. 插件 (Plugin)
- **静态插件**：定义 `struct XxxPlugin { void build(App& app); }`，通过 `app.add_plugin(XxxPlugin{});` 注册。
  - 构建期展开，运行时无插件对象残留，零开销。
  - 内置插件：`HierarchyPlugin`（父子层级 + 级联删除）、`CSVWriterPlugin`（Metric → CSV 导出）。
  - 示例：
    ```cpp
    struct PhysicsPlugin {
        void build(App& app) {
            app.add_resource(PhysicsConfig{...});
            app.system("PhysicsStep").run(compute_physics).build();
        }
    };
    app.add_plugin(PhysicsPlugin{});
    ```
