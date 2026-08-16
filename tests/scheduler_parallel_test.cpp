#include <gtest/gtest.h>
#include <atomic>
#include <thread>

import elysia.world;
import elysia.schedule;
import elysia.entity;

using namespace elysia;

struct Val { int x; };

TEST(ElysiaScheduler, ParallelApplyDeferred) {
    World world;
    Scheduler scheduler;

    std::atomic<int> stage{0};

    // System A: Runs first
    scheduler.system("SystemA")
        .run([&](World*) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stage.fetch_add(1);
        })
        .build();

    // Sync Point: Define a unique sync node
    scheduler.system("Sync1")
        .kind(schedule::SpecialSystemKind::ApplyDeferred)
        .after("SystemA")
        .build();

    // System B: Depends on Sync Point
    scheduler.system("SystemB")
        .run([&](World*) {
            // If sync works, stage must be 1 here
            EXPECT_EQ(stage.load(), 1);
            stage.fetch_add(1);
        })
        .after("Sync1")
        .build();

    // Run via Taskflow
    auto exec = TaskflowExecutor::build_from(scheduler);
    exec->run(&world);

    EXPECT_EQ(stage.load(), 2);
}

TEST(ElysiaScheduler, ParallelStructuralConsistency) {
    World world;
    auto e = world.spawn().add(Val{0}).entity;
    
    Scheduler scheduler;
    struct Tag1 {  }; 
    struct Tag2 {  };

    scheduler.system("AddTag1")
        .run([e](CommandBuffer& cmd) {
            cmd.insert(e, Tag1{});
        })
        .build();

    scheduler.system("AddTag2")
        .run([e](CommandBuffer& cmd) {
            cmd.insert(e, Tag2{});
        })
        .build();

    // Sync point: Flush commands from AddTag1 and AddTag2
    scheduler.system("Flush")
        .kind(schedule::SpecialSystemKind::ApplyDeferred)
        .after("AddTag1")
        .after("AddTag2")
        .build();

    auto exec = TaskflowExecutor::build_from(scheduler);
    exec->run(&world);

    // Verify both tags added
    EXPECT_NE(world.get_component<Tag1>(e), nullptr);
    EXPECT_NE(world.get_component<Tag2>(e), nullptr);
}
