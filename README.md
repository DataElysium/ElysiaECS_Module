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
