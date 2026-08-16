# Elysia ECS High-Speed API Reference (Cheat Sheet)

## 1. Query
- **Ad-hoc Query**: `world.query<T>().each(...)` — Automatically calls `update_query` internally, suitable for one-off iterations.
- **Member Query**: Storing a Query as a member in Functor systems. You must manually call `w.update_query(member_q);` every frame (⚠️ Mandatory for Functors to avoid redundant matching overhead).

## 2. CommandBuffer
- **Modify Components**: `cmd.insert(e, Data{})` — No Index binding required, applies only to existing entities.
- **Spawn Entities**: `cmd.spawn()` — Requires an EntityIndex binding:
  - Method 1 (Recommended): `CommandBuffer cmd(&world.index());`
  - Method 2: `CommandBuffer cmd; cmd.set_index(&world.index());`

## 3. Archive
- **Full Mirror**: `RawArchive::pack(world)` — Returns `Result<std::vector<char>>`, a binary blob. POD exclusively, GB/s level performance.
  - Unpack: `RawArchive::unpack<T...>(world, data)`.
- **Structured Snapshot**: `AuroraArchive::create(world, reg, {Format::Csv})` — Builds an in-memory snapshot (`WorldArchive`) from a `SnapshotRegistry`, supporting `Columnar` / `Csv` / `Raw` formats.
  - Load: `load_aurora(world, cmd, reg, archive)`.

## 4. Scheduler
- **Topological Sort**: Use `before()` / `after()` to establish system dependencies; the executor automatically performs DAG sorting.
- **Parallel Safety**: Acquiring a `CommandBuffer&` in a system Lambda is the safest way to perform parallel modifications (deferred operations, uniformly applied at frame end).
- **Functor Systems**: `app.system("Name").run(MySystem{...}).build();` — System as a callable object, supporting state persistence.

## 5. Plugin
- **Static Plugin**: Define `struct XxxPlugin { void build(App& app); }`, and register via `app.add_plugin(XxxPlugin{});`.
  - Expanded at build time, leaving no plugin object residue at runtime. Zero overhead.
  - Built-in Plugins: `HierarchyPlugin` (Parent-child hierarchy + cascading deletion), `CSVWriterPlugin` (Metric → CSV export).
  - Example:
    ```cpp
    struct PhysicsPlugin {
        void build(App& app) {
            app.add_resource(PhysicsConfig{...});
            app.system("PhysicsStep").run(compute_physics).build();
        }
    };
    app.add_plugin(PhysicsPlugin{});
    ```
