# Elysia ECS (C++20 Module Version)

**Elysia ECS** is a high-performance, modern Entity-Component-System engine written completely with C++20 Modules. It combines the extreme performance of Structure of Arrays (SoA) chunk-iteration with an incredibly ergonomic, Bevy/Flecs-inspired Lambda API, while leveraging the rapid compilation times and isolation of C++ Modules.

## 🚀 Quick Start (Showcase)

Elysia uses powerful template metaprogramming to automatically deduce your system dependencies. You don't need boilerplate structs—just write pure lambdas!

```cpp
#include <iostream>
import elysia;

using namespace elysia;

// 1. Define your plain-old-data components
struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct TimeConfig { float dt; }; // Global resource

int main() {
    App app;

    // 2. Startup System (Runs once at the beginning)
    app.add_startup_system("Init", [](World* w) {
        w->resources().add(TimeConfig{1.0f}); // Register global resource

        // Fluent Entity Builder
        w->spawn()
            .add(Position{0.0f, 0.0f})
            .add(Velocity{1.0f, 2.0f});
            
        w->spawn()
            .add(Position{10.0f, 10.0f})
            .add(Velocity{-1.0f, 0.0f});
    });

    // 3. Lambda Auto-Query System
    //    Elysia automatically builds a query for Entities having BOTH Position and Velocity.
    //    It also seamlessly injects the 'TimeConfig' global resource!
    app.system("Move").run([](Position& p, const Velocity& v, Res<TimeConfig> time) {
        p.x += v.dx * time->dt;
        p.y += v.dy * time->dt;
    }).build(); // <-- Don't forget .build() to finalize registration!

    // 4. CommandBuffer Injection
    //    Safely mutate the world (e.g., despawning) in parallel. Deferred to the end of the stage.
    app.system("BoundsCheck").after("Move").run([](CommandBuffer& cmd, Entity e, const Position& p) {
        if (p.x < 0.0f || p.y < 0.0f) {
            cmd.despawn(e);
        }
    }).build();

    // 5. Main Execution Loop
    for (int frame = 0; frame < 3; ++frame) {
        app.update(); // Executes the DAG of systems
    }

    return 0;
}
```

## ✨ Key Features

1. **C++20 Modules (`import elysia;`)**: No more `#include` hell. Experience lightning-fast incremental builds and clean namespaces.
2. **Lambda Auto-Query**: The scheduler automatically builds queries based on your lambda signature.
3. **Resource Injection (`Res<T>`)**: Transparently request global resources directly in your system signature.
4. **Command Buffers**: Thread-safe, deferred world mutations (`cmd.spawn`, `cmd.despawn`, `cmd.add`, `cmd.remove`).
5. **DAG System Scheduling**: Use `.after("SystemName")` and `.before("SystemName")` to build a complex execution graph.
6. **High-Performance Bulk Iteration**: Need to squeeze every cycle out of the CPU? Bypass lambdas and iterate directly over SoA memory chunks using `BulkSystems` (see `docs/systems_api.md`).

## 📚 Documentation

Detailed guides and API references can be found in the `docs/` folder:
- [Systems API (Lambda, Bulk, Functor, etc.)](docs/systems_api.md)
- [Cheat Sheet](docs/cheat_sheet.md)
- [Scheduling Logic](docs/scheduling_logic.md)
- [User Guide](docs/USER_GUIDE.md)

## 🛠️ Building

This project is built using [xmake](https://xmake.io).

```bash
# Configure the project (Uses Clang for C++20 Module support)
xmake f -c 

# Build and run the Hello World example
xmake build ElysiaHelloWorld
xmake run ElysiaHelloWorld

# Build and run the Boids demo
xmake build elysia_boids
xmake run elysia_boids
```


## Runtime component queries

`DynamicQuery` selects columns and filters by registered component IDs. It does
not require a C++ component definition or a reflection registry:

```cpp
const auto id = elysia::fnv1a_64("plugin::Position");
// A plugin/loader can supply this descriptor and lifecycle hooks instead.
world.graph().registry().register_opaque(
    id, "plugin::Position", 3 * sizeof(double), alignof(double));

elysia::DynamicQuery query({.columns = {id}});
world.update_query(query);
query.each_chunk([](const elysia::DynamicChunkView& chunk) {
    // chunk.entities: entity IDs in row order
    // chunk.columns: selected columns in descriptor order
    // Each column provides its ID, address, byte stride, and alignment.
    // Interpret its bytes using your plugin's schema or native code.
});
```

`with` and `without` contain additional required and excluded IDs. Tags belong
in filters, not selected data columns. Disabled entities are excluded by default;
set `include_inactive = true` or explicitly require `DisabledTag` to include them.
Every explicit ID must already be registered. Unknown IDs, duplicate columns,
and contradictory descriptor filters report errors.

Call `world.update_query(query)` before each traversal to discover new archetypes.
The cached query rebinds when updated against a different world's registry.
Call `query.reset()` after moving/replacing a world or changing inherited external
filters. The query and borrowed views must not outlive the world they reference.

For erased callers, `each_chunk(visitor, context)` accepts a plain function pointer
with signature `void(void*, const DynamicChunkView&)`. Column/entity views are
valid only during the callback. Structural changes must be deferred until
iteration ends. Concurrent access still requires the caller's synchronization;
the descriptor does not infer access conflicts or schedule work.

This API uses C++ view types; it is not a versioned C ABI. A DLL-facing facade can
translate its own plain descriptors and callbacks without adding reflection to
the ECS.


### Runtime adapter example

Run `xmake run -P . runtime_trait_example` after building
`xmake build -P . runtime_trait_example` from this repository.

`examples/runtime_trait.cpp` is an example-local `RuntimeTrait{&world}` prototype.
Start at `main()` for the host flow, then inspect `plugin::samples_type()` for
custom deep-copy, move, and destruction callbacks. Runtime resources live in an
external map owned by a single host resource; they are not exposed through
`Res<T>`. No `World` APIs or core lifecycle metadata are changed. The plugin
namespace models a plugin boundary in one executable; this is not a DLL loader
or a C ABI.
