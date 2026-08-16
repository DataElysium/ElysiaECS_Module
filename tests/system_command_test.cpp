#include <gtest/gtest.h>
#include <vector>

import elysia.world;
import elysia.schedule;
import elysia.entity;

using namespace elysia;

struct Val { int x; };
struct Tag {};

TEST(SystemBuilder, CommandBufferInjection) {
    World world;
    auto e = world.spawn().add(Val{1}).entity;

    Scheduler scheduler;
    
    // System with CommandBuffer (Must be first arg)
    scheduler.system("DeferSys")
        .run([](CommandBuffer& cmd, Val& v, Entity current_e) {
            v.x += 1;
            // Defer adding Tag to this entity
            cmd.insert(current_e, Tag{});
        })
        .build();

    // Run
    auto exec = SerialExecutor::build_from(scheduler);
    exec->run(&world);

    // Verify
    // 1. Existing entity updated
    auto* val = world.get_component<Val>(e);
    EXPECT_EQ(val->x, 2);

    // 2. Tag added (Deferred)
    // Need to update query/world state if structural change happened?
    // World::submit does structural changes immediately.
    // So get_component should work.
    auto* tag = world.get_component<Tag>(e);
    ASSERT_NE(tag, nullptr);
}