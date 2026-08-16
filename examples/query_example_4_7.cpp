/**
 * @file query_example_4_7.cpp
 * @brief Official Example: Elysia Fluent API & Bulk Iteration
 * 
 * Features:
 * 1. Fluent API: world.query<...>().filter<...>().iter(...)
 * 2. Incremental Updates: Query caching for O(0) overhead on stable structures.
 * 3. Resource Handles: Accessing global data via Res<T>.
 * 4. Entity Handles: Accessing Entity ID via Entity argument.
 */

#include <iostream>
#include <format> // C++20 formatting

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;

// --- Domain Components ---
struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Mass { float value; };

// --- Tags ---
struct StaticTag {}; // Empty struct
struct ActiveTag {};

// --- Resources ---
struct PhysicsConfig {
    float time_step = 0.016f;
    float global_friction = 0.99f;
};

int main() {
    World world;

    // 1. Setup Resources
    world.resources().add(PhysicsConfig{ 0.016f, 0.98f });

    // 2. Setup Entities
    // Entity A: Active
    auto e1 = world.spawn()
        .add(Position{0, 0})
        .add(Velocity{10, 5})
        .add(ActiveTag{})
        .entity;

    // Entity B: Static
    world.spawn()
        .add(Position{100, 100})
        .add(StaticTag{});

    std::cout << "--- Elysia Physics Simulation ---" << std::endl;

    // 3. Define Query (Hoist out of loop for caching)
    // Selects: Position (RW), Velocity (RW), PhysicsConfig (Read via Res)
    // Filters: Must have ActiveTag, Must NOT have StaticTag
    // Also captures Entity ID to print.
    auto sys_physics = world.query<Entity, Position, Velocity, Res<PhysicsConfig>>()
                            .filter<With<ActiveTag>, Without<StaticTag>>();

    // 4. Game Loop
    for(int i=0; i<5; ++i) {
        std::cout << "[Frame " << i << "]" << std::endl;

        // Use .each() for simple logic (Lambda per entity)
        // Note: Res<T> is passed by value (it's a handle), use -> to access members
        sys_physics.each([&](Entity e, Position& p, Velocity& v, Res<PhysicsConfig> cfg) {
            p.x += v.dx * cfg->time_step;
            p.y += v.dy * cfg->time_step;
            v.dx *= cfg->global_friction;
            v.dy *= cfg->global_friction;

            std::cout << "  Entity " << e.id() << ": Pos(" << p.x << ", " << p.y << ")" << std::endl;
        });
    }

    return 0;
}