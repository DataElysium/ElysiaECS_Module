# Elysia ECS Archive User Manual

**Elysia ECS Archive** is a high-performance, tiered archival system designed for extreme-scale simulations. It prioritizes data integrity and I/O throughput, offering three distinct storage tiers to balance performance and flexibility.

## 1. The 3-Layer Pipeline Architecture

The system utilizes a URI-driven resolution pipeline:
1.  **Resolver Layer**: Locates data via URIs (`file://`, `embed://`).
2.  **Middleware Layer**: Handles transport encoding (e.g., `Base64` decoding for TOML embedding, `UTF-8` for text).
3.  **Codec Layer**: Performs the final deserialization into ECS Archetypes.

---

## 2. Storage Tiers (The Three Paths)

### 🚀 Tier 0: Full World Snapshot (`RawArchive`)
*   **Method**: Direct memory mirroring of the entire `World`.
*   **Performance**: Extreme (GB/s).
*   **Constraint**: ⚠️ **POD ONLY**. Only components that are `std::is_trivially_copyable` are safe. Any component with custom destructors, pointers, or heap-allocated resources will fail the safety scan.

### 🛠️ Tier 1: Per-Archetype Binary (`RawTableCodec`)
*   **Method**: High-performance binary dump of individual Archetype memory blocks.
*   **Use Case**: Level streaming, patching entities from multiple files.
*   **Integration**: Used via `AuroraArchive` with `Format::Raw`.

### 📝 Tier 2: Structured & Interchangeable (`CSV` / `Columnar`)
*   **Method**: Self-describing data via JSON-like trees (`reflect::Generic`).
*   **Use Case**: Human-readable configuration, cross-system data exchange (e.g., National Grid GB2312 via Middleware).
*   **Flexibility**: Supports non-POD types via **Proxy Patterns**.

---

## 3. The "POD-Only" Safety Guard

To prevent silent memory corruption, the **Raw (Tier 0/1)** paths perform a safety check:
- If a component has a `dtor` (destructor) or `move` hook registered, the system will **refuse to pack** it in Raw mode.
- **Solution**: For complex types, use a **Proxy Type** or switch to **Structured Path (Tier 2)**.

---

## 4. Entity Patching (Multi-File Loading)

Elysia supports merging data from multiple sources into a single entity.
*   **Logic**: If multiple `ResourceEntry`s (from different files) target the same Entity ID, the `CommandBuffer` will automatically fuse the components during the submission phase.
*   **Example**: Load physics data from `physics.raw` and logic state from `logic.csv` into the same set of entities.

---

## 5. Basic Usage

### Standard CSV Export (Default)
```cpp
auto snapshot = AuroraArchive::create(world, reg); // Default is CSV
auto toml = reflect::write_toml(snapshot);
```

### High-Performance Binary Export
```cpp
AuroraArchive::Config config { AuroraArchive::Config::Format::Raw };
auto snapshot = AuroraArchive::create(world, reg, config);
```

### Loading with URI Resolution
```cpp
World world2;
CommandBuffer cmd(&world2.index());
auto res = archive::load_aurora(world2, cmd, reg, snapshot);
world2.submit(cmd); // All entities materialized and patched
```

---

## 6. Advanced: Encoding & Middleware
You can extend the pipeline to handle legacy encodings or compression:
*   **Encoding: `base64`**: Automatically handled for binary blobs in text manifests.
*   **Encoding: `utf-8`**: Standard for CSV.
*   **Future**: Add `gzip` or `gb2312` by implementing a new `Middleware`.
