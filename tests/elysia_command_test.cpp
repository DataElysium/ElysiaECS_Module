#include <gtest/gtest.h>
#include <string>

import elysia.world;
import elysia.meta;
import elysia.entity;

using namespace elysia;

struct Pos { float x, y; };
struct Vel { float dx, dy; };

TEST(ElysiaCommand, LocalFusion) {
    World world;
    CommandBuffer cmd(&world.index());
    
    // Manual ID reservation (simulated)
    Entity e = world.spawn().entity; 
    // We already spawned it to get ID. Now let's "respawn" it via command buffer?
    // World::spawn_at overwrites/moves if alive.
    // If e is already alive in [Empty], spawn_bundle will move it to [Pos, Vel].
    
    cmd.spawn(e);
    cmd.insert(e, Pos{10, 20});
    cmd.insert(e, Vel{1, 1});
    
    world.submit(cmd);
    
    Pos* p = world.entity(e).get<Pos>();
    Vel* v = world.entity(e).get<Vel>();
    
    ASSERT_NE(p, nullptr);
    ASSERT_NE(v, nullptr);
    EXPECT_FLOAT_EQ(p->x, 10);
    EXPECT_FLOAT_EQ(v->dx, 1);
}

TEST(ElysiaCommand, RiskyCall) {
    World world;
    CommandBuffer cmd(&world.index());
    
    int side_effect = 0;
    // Lambda must accept void* and cast it
    cmd.call([&](void* w_ptr) {
        World& w = *static_cast<World*>(w_ptr);
        (void)w; // Unused
        side_effect = 42;
    });
    
    world.submit(cmd);
    EXPECT_EQ(side_effect, 42);
}

TEST(ElysiaCommand, InterleavedOps) {
    World world;
    CommandBuffer cmd(&world.index());
    
    // Pre-spawn to get valid IDs
    Entity e1 = world.spawn().entity;
    Entity e2 = world.spawn().entity;
    
    cmd.spawn(e1);
    cmd.insert(e1, Pos{1, 1});
    
    cmd.spawn(e2);
    cmd.insert(e2, Vel{2, 2});
    
    world.submit(cmd);
    
    EXPECT_NE(world.entity(e1).get<Pos>(), nullptr);
    EXPECT_EQ(world.entity(e1).get<Vel>(), nullptr);
    
    EXPECT_EQ(world.entity(e2).get<Pos>(), nullptr);
    EXPECT_NE(world.entity(e2).get<Vel>(), nullptr);
}