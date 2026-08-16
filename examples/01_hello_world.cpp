#include <iostream>

import elysia;

using namespace elysia;

// Define simple components
struct Position { float x, y; };
struct Velocity { float dx, dy; };

// Define a global resource
struct TimeConfig { float dt; };

int main() {
    App app;

    // 1. Startup system: runs once before the main loop
    app.add_startup_system("Init", [](World* w) {
        // Add a global resource
        w->resources().add(TimeConfig{1.0f});

        // Spawn entities using the fluent builder pattern
        w->spawn()
            .add(Position{0.0f, 0.0f})
            .add(Velocity{1.0f, 2.0f});
            
        w->spawn()
            .add(Position{10.0f, 10.0f})
            .add(Velocity{-1.0f, 0.0f});

        std::cout << "[Init] Spawned 2 entities.\n";
    });

    // 2. Auto-Query System: Lambda signature automatically deduces the required components!
    //    It also injects the 'TimeConfig' resource seamlessly.
    app.system("Move").run([](Position& p, const Velocity& v, Res<TimeConfig> time) {
        p.x += v.dx * time->dt;
        p.y += v.dy * time->dt;
    }).build(); // <-- Essential: call .build() to finalize system registration

    // 3. CommandBuffer Injection: Safely queue structural changes (like despawning)
    //    This is thread-safe and deferred until the end of the stage.
    app.system("BoundsCheck").after("Move").run([](CommandBuffer& cmd, Entity e, const Position& p) {
        if (p.x < 0.0f || p.y < 0.0f) {
            std::cout << "[BoundsCheck] Entity " << e.id() << " went out of bounds. Despawning.\n";
            cmd.despawn(e);
        }
    }).build();

    // 4. Run the application
    for (int frame = 0; frame < 3; ++frame) {
        std::cout << "\n--- Frame " << frame << " ---\n";
        
        // update() executes the DAG of systems. 
        // The first update() will automatically run the startup systems.
        app.update(); 

        // 5. Ad-hoc query: Useful for one-off reads outside of systems
        app.world().query<Entity, const Position>().each([](Entity e, const Position& p) {
            std::cout << "  Entity " << e.id() << " is at (" << p.x << ", " << p.y << ")\n";
        });
    }

    return 0;
}
