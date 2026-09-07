#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <stdexcept>

import elysia.world;
import elysia.schedule;
import elysia.entity;

namespace elysia_test::scheduler_parallel_test {

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


// Observe execution without adding synchronization dependencies between systems.
struct ExclusiveProbe {
    std::mutex mutex;
    int active = 0, exclusive = 0;
    bool overlap = false;
    std::atomic<int> entered{0};
    void work(bool is_exclusive, int expected) {
        {
            std::lock_guard lock(mutex);
            overlap |= exclusive != 0 || (is_exclusive && active != 0);
            ++active;
            exclusive += is_exclusive;
        }
        ++entered;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
        while (entered.load() < expected && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        {
            std::lock_guard lock(mutex);
            --active;
            exclusive -= is_exclusive;
        }
    }
};

TEST(TaskflowExclusive, ExplicitAndWorldSystemsExcludeUnrelatedWork) {
    if (std::thread::hardware_concurrency() < 2) GTEST_SKIP();
    World world;
    ExclusiveProbe probe;
    Scheduler scheduler;
    scheduler.system("Explicit").exclusive().run([&]() { probe.work(true, 3); }).build();
    scheduler.system("World").run([&](World*) { probe.work(true, 3); }).build();
    scheduler.system("Unrelated").run([&]() { probe.work(false, 3); }).build();
    auto exec = TaskflowExecutor::build_from(scheduler);
    exec->run(&world);
    EXPECT_EQ(probe.entered.load(), 3);
    EXPECT_FALSE(probe.overlap);
}

TEST(TaskflowExclusive, ApplyDeferredExcludesUnrelatedWork) {
    if (std::thread::hardware_concurrency() < 2) GTEST_SKIP();
    World world;
    ExclusiveProbe probe;
    Scheduler scheduler;
    scheduler.system("Queue").run([&](WorldView, CommandBuffer* cmd) {
        cmd->call([&](void*) { probe.work(true, 2); });
    }).build();
    scheduler.system("Flush").after("Queue").kind(schedule::SpecialSystemKind::ApplyDeferred).build();
    scheduler.system("Unrelated").after("Queue").run([&]() { probe.work(false, 2); }).build();
    TaskflowExecutor::build_from(scheduler)->run(&world);
    EXPECT_EQ(probe.entered.load(), 2);
    EXPECT_FALSE(probe.overlap);
}

// B must be allowed to run while C is still active, despite belonging to a later
// topological layer. A layer-wide barrier would make the rendezvous time out.
TEST(TaskflowExclusive, PreservesParallelBranchesAcrossLayersAndAfterBarrier) {
    if (std::thread::hardware_concurrency() < 2) GTEST_SKIP();
    World world;
    Scheduler scheduler;
    std::atomic<int> before{0}, completed{0}, after{0};
    bool exclusive_done = false;
    auto rendezvous = [](std::atomic<int>& entered) {
        ++entered;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (entered.load() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        EXPECT_EQ(entered.load(), 2);
    };
    scheduler.system("A").run([]() {}).build();
    scheduler.system("B").after("A").run([&]() { rendezvous(before); ++completed; }).build();
    scheduler.system("C").run([&]() { rendezvous(before); ++completed; }).build();
    scheduler.system("X").after("B").after("C").exclusive().run([&]() {
        EXPECT_EQ(completed.load(), 2);
        exclusive_done = true;
    }).build();
    for (auto name : {"D", "E"})
        scheduler.system(name).after("X").run([&]() {
            EXPECT_TRUE(exclusive_done);
            rendezvous(after);
        }).build();
    TaskflowExecutor::build_from(scheduler)->run(&world);
    EXPECT_EQ(after.load(), 2);
}

TEST(TaskflowExclusive, FailureSkipsConsecutiveExclusiveSystems) {
    World world;
    Scheduler scheduler;
    bool fail = true;
    int completed = 0;
    scheduler.system("Fail").run([&]() { if (fail) throw std::runtime_error("stop"); }).build();
    scheduler.system("X").after("Fail").exclusive().run([&]() { ++completed; }).build();
    scheduler.system("Y").after("X").exclusive().run([&]() { ++completed; }).build();
    scheduler.system("Z").after("Y").run([&]() { ++completed; }).build();
    auto exec = TaskflowExecutor::build_from(scheduler);
    EXPECT_THROW(exec->run(&world), std::runtime_error);
    EXPECT_EQ(completed, 0);
    fail = false;
    EXPECT_NO_THROW(exec->run(&world));
    EXPECT_EQ(completed, 3);
}

TEST(TaskflowExclusive, EmptyGraphRunsAndCyclesAreRejected) {
    World world;
    Scheduler empty;
    EXPECT_NO_THROW(TaskflowExecutor::build_from(empty)->run(&world));
    Scheduler cyclic;
    cyclic.system("A").after("B").exclusive().run([]() {}).build();
    cyclic.system("B").after("A").run([]() {}).build();
    EXPECT_THROW(TaskflowExecutor::build_from(cyclic), std::logic_error);
}

} // namespace elysia_test::scheduler_parallel_test
