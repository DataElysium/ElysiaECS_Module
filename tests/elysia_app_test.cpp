#include <gtest/gtest.h>
#include <vector>

import elysia;

using namespace elysia;

struct TagA {};
struct TagB {};

TEST(ElysiaApp, ObserverOnAdd) {
    App app;
    std::vector<Entity> observed_entities;

    // 1. Register Observer
    app.observer<OnAdd, TagA>().run([&](Entity e) {
        observed_entities.push_back(e);
    });

    // 2. Add system that spawns entities with TagA
    app.system("Spawner")
        .run([](World* w) {
            auto& cmd = w->commands().buffer_a();
            Entity e = w->spawn().entity;
            cmd.insert(e, TagA{});
            w->submit(cmd); // Ensure commands are processed
        })
        .build();

    // 3. First Update
    app.update();

    // Verification:
    // One entity was spawned, TagA was added via CommandBuffer.
    // The observer should have been triggered during CommandBuffer settlement.
    EXPECT_EQ(observed_entities.size(), 1);
}

TEST(ElysiaApp, ObserverFusionSpawn) {
    App app;
    int call_count = 0;

    app.observer<OnAdd, TagA>().run([&](Entity e) {
        call_count++;
    });

    // System using CommandBuffer to spawn with fused components
    app.system("FusedSpawner")
        .run([](World* w) {
            auto& cmd = w->commands().buffer_a();
            Entity e = w->spawn().entity;
            cmd.insert(e, TagA{}); // These will be fused
            cmd.insert(e, TagB{});
            w->submit(cmd);
        })
        .build();

    app.update();

    // Even if fused, OnAdd for TagA should trigger.
    EXPECT_EQ(call_count, 1);
}

TEST(ElysiaApp, MultipleObservers) {
    App app;
    int count_a = 0;
    int count_b = 0;

    app.observer<OnAdd, TagA>().run([&](Entity e) { count_a++; });
    app.observer<OnAdd, TagB>().run([&](Entity e) { count_b++; });

    app.system("DualSpawner")
        .run([](World* w) {
            auto& cmd = w->commands().buffer_a();
            Entity e1 = w->spawn().entity;
            cmd.insert(e1, TagA{});
            
            Entity e2 = w->spawn().entity;
            cmd.insert(e2, TagB{});
            w->submit(cmd);
        })
        .build();

    app.update();

    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);
}
