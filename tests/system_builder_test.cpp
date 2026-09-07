#include <gtest/gtest.h>
#include <atomic>
#include <vector>
#include <memory>
#include <stdexcept>

import elysia.world;
import elysia.schedule;
import elysia.entity;
import elysia.query;

namespace elysia_test::system_builder_test {

using namespace elysia;

struct Val { int x; };

TEST(SystemBuilder, UltimateApiCheck) {
    World game_world;
    for(int i=0; i<10; ++i) game_world.spawn().add(Val{0});

    Scheduler scheduler;
    
    // 1. Each System (High Level)
    scheduler.system("EachSys")
        .run([](Val& v) {
            v.x += 1;
        })
        .build();

    // 2. Bulk System (Low Level Optimization)
    scheduler.system("BulkSys")
        .run([](size_t count, Val* v) {
            for(size_t i=0; i<count; ++i) v[i].x += 10;
        })
        .after("EachSys") // Run after EachSys
        .build();

    // 3. World System (Exclusive/Global)
    int world_sys_runs = 0;
    scheduler.system("WorldSys")
        .run([&](World* w) {
            world_sys_runs++;
            // Check intermediate state (should be 11: 0 + 1 + 10)
            // But we don't know if this runs after BulkSys unless we set dependency.
            // Let's make it run LAST.
        })
        .after("BulkSys")
        .build();

    // Run via Executor
    auto executor = SerialExecutor::build_from(scheduler);
    executor->run(&game_world);

    // Verify
    // 1. All entities should be 11
    int sum = 0;
    game_world.query<Val>().each([&](Val& v) { sum += v.x; });
    EXPECT_EQ(sum, 110); // 10 entities * 11

    // 2. World System ran once
    EXPECT_EQ(world_sys_runs, 1);
}

// WorldView functor systems stay Parallel and receive a parallel-safe wrapper.
TEST(SystemBuilder, WorldViewSystemRun) {
    World world;
    for (int i = 0; i < 5; ++i) world.spawn().add(Val{0});

    Scheduler scheduler;
    std::atomic<int> wv_runs{0};
    std::atomic<int> wv_cmd_runs{0};

    // WorldViewSystem — parallel, no CommandBuffer
    struct WVSys {
        Query<Val> q;
        std::atomic<int> *counter;
        void operator()(WorldView w) {
            w.update_query(q);
            q.each(w.raw(), [](Val &v) { v.x += 1; });
            counter->fetch_add(1);
        }
    };
    WVSys sys;
    sys.counter = &wv_runs;
    scheduler.system("WVSys").run(std::move(sys)).build();

    // WorldViewCmdSystem — parallel, receives CommandBuffer*
    struct WVCmdSys {
        Query<Val> q;
        std::atomic<int> *counter;
        void operator()(WorldView w, CommandBuffer *) {
            w.update_query(q);
            q.each(w.raw(), [](Val &v) { v.x += 10; });
            counter->fetch_add(1);
        }
    };
    WVCmdSys cmd_sys;
    cmd_sys.counter = &wv_cmd_runs;
    scheduler.system("WVCmdSys").after("WVSys").run(std::move(cmd_sys)).build();

    auto exec = SerialExecutor::build_from(scheduler);
    exec->run(&world);

    EXPECT_EQ(wv_runs.load(), 1);
    EXPECT_EQ(wv_cmd_runs.load(), 1);
    int sum = 0;
    world.query<Val>().each([&](Val &v) { sum += v.x; });
    EXPECT_EQ(sum, 55); // 5 entities * (1 + 10)
}

TEST(SystemBuilder, CallableTransfersOnce) {
    World world;
    Scheduler first, second;
    auto e = world.spawn().add(Val{0}).entity;
    auto builder = schedule::SystemBuilder("Owned").run(
        [values = std::vector<int>{7}](Val& v) { if (!values.empty()) v.x += values[0]; });
    builder.build(first);
    EXPECT_THROW(builder.build(first), std::logic_error);
    EXPECT_THROW(builder.build(second), std::logic_error);
    int systems = 0;
    second.meta_world().query<schedule::SysName>().each([&](auto&) { ++systems; });
    EXPECT_EQ(systems, 0);
    auto consumed_copy = builder;
    EXPECT_THROW(consumed_copy.build(second), std::logic_error);
    SerialExecutor::build_from(first)->run(&world);
    EXPECT_EQ(world.get_component<Val>(e)->x, 7);

    builder.run([](Val& v) { v.x += 3; });
    EXPECT_NO_THROW(builder.build(second));
}

TEST(SystemBuilder, CallableOutlivesBuilderAndUnusedCopiesAreIndependent) {
    World a, b;
    auto ea = a.spawn().add(Val{0}).entity;
    auto eb = b.spawn().add(Val{0}).entity;
    std::weak_ptr<int> lifetime;
    {
        Scheduler first, second;
        {
            auto owned = std::make_shared<int>(7);
            lifetime = owned;
            auto builder = schedule::SystemBuilder("Owned").run(
                [owned, calls = 0](Val& v) mutable { v.x += *owned + calls++; });
            auto copy = builder;
            builder.build(first);
            copy.build(second);
        }
        EXPECT_FALSE(lifetime.expired());
        auto exec = SerialExecutor::build_from(first);
        exec->run(&a);
        exec->run(&a);
        SerialExecutor::build_from(second)->run(&b);
        EXPECT_EQ(a.get_component<Val>(ea)->x, 15);
        EXPECT_EQ(b.get_component<Val>(eb)->x, 7);
    }
    EXPECT_TRUE(lifetime.expired());
}

} // namespace elysia_test::system_builder_test
