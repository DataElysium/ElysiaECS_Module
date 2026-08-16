#include <gtest/gtest.h>
#include <string>
#include <vector>

import elysia.world;
import elysia.schedule;
import elysia.entity;

using namespace elysia;

struct Score { int value = 0; };

TEST(ElysiaScheduler, MultiWorldIsolation) {
    // 1. Define a REUSABLE system descriptor (Blueprint)
    // Note: It's NOT built yet, just defined.
    auto score_sys = schedule::SystemBuilder("ScoreSys")
        .run([](Score& s) {
            s.value += 10;
        });

    // 2. Setup World A
    World world_A;
    Scheduler sched_A;
    auto eA = world_A.spawn().add(Score{0}).entity;
    score_sys.build(sched_A); // Instantiate into World A

    // 3. Setup World B
    World world_B;
    Scheduler sched_B;
    auto eB = world_B.spawn().add(Score{100}).entity;
    score_sys.build(sched_B); // Instantiate into World B (Fresh Query Instance!)

    // 4. Run A
    auto exec_A = SerialExecutor::build_from(sched_A);
    exec_A->run(&world_A);
    
    // Verify A updated, B stayed same
    EXPECT_EQ(world_A.get_component<Score>(eA)->value, 10);
    EXPECT_EQ(world_B.get_component<Score>(eB)->value, 100);

    // 5. Run B (Twice)
    auto exec_B = SerialExecutor::build_from(sched_B);
    exec_B->run(&world_B);
    exec_B->run(&world_B);

    // Verify B updated, A stayed same
    EXPECT_EQ(world_A.get_component<Score>(eA)->value, 10);
    EXPECT_EQ(world_B.get_component<Score>(eB)->value, 120);
    
    // Final sanity check on use_counts? 
    // We don't expose internal query ptrs, but if they shared state, B would have seen A's entities.
}

TEST(ElysiaScheduler, CrossWorldDeferred) {
    // Verify that CommandBuffers are also isolated
    struct TagA {};
    struct TagB {};

    auto spawn_sys = schedule::SystemBuilder("SpawnSys")
        .run([](CommandBuffer& cmd, Score& s, Entity e) {
            if (s.value > 50) cmd.insert(e, TagB{});
            else cmd.insert(e, TagA{});
        });

    World world_A; Scheduler sched_A;
    auto eA = world_A.spawn().add(Score{10}).entity; // Should get TagA
    spawn_sys.build(sched_A);

    World world_B; Scheduler sched_B;
    auto eB = world_B.spawn().add(Score{90}).entity; // Should get TagB
    spawn_sys.build(sched_B);

    SerialExecutor::build_from(sched_A)->run(&world_A);
    SerialExecutor::build_from(sched_B)->run(&world_B);

    EXPECT_NE(world_A.get_component<TagA>(eA), nullptr);
    EXPECT_EQ(world_A.get_component<TagB>(eA), nullptr);

    EXPECT_EQ(world_B.get_component<TagA>(eB), nullptr);
    EXPECT_NE(world_B.get_component<TagB>(eB), nullptr);
}
