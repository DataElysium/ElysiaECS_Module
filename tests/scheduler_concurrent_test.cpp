#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

import elysia.world;
import elysia.schedule;
import elysia.entity;

using namespace elysia;

// =============================================================================
// Helper: detects whether two systems ran concurrently
// Uses a "simultaneous occupancy" counter — if two systems are inside their
// enter/leave window at the same time, in_flight will reach 2.
// =============================================================================
struct OverlapDetector {
    std::atomic<int> in_flight{0};
    std::atomic<bool> saw_overlap{false};

    void enter(unsigned int sleep_ms = 20) {
        in_flight.fetch_add(1, std::memory_order_acq_rel);
        // Hold the slot long enough for a concurrent system to also enter.
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        if (in_flight.load(std::memory_order_acquire) > 1)
            saw_overlap.store(true, std::memory_order_release);
    }
    void leave() {
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
};

// =============================================================================
// Test 1: WorldMutSystem (World*)  →  auto-Exclusive  →  MUST be serial
// =============================================================================
// Two systems both receive mutable World*. Without auto-Exclusive they would
// run in the same wave concurrently. With auto-Exclusive, ForkUnion must
// serialize them so saw_overlap remains false.
TEST(ElysiaSchedulerConcurrent, WorldMutSystemAutoExclusive) {
    OverlapDetector det;

    Scheduler scheduler;
    scheduler.system("MutA")
        .run([&](World*) { det.enter(); det.leave(); })
        .build();
    scheduler.system("MutB")
        .run([&](World*) { det.enter(); det.leave(); })
        .build();
    // No dependencies — both land in the same wave (or adjacent).

    World world;
    auto exec = ForkUnionExecutor::build_from(scheduler);
    exec->run(&world);

    EXPECT_FALSE(det.saw_overlap.load())
        << "WorldMutSystem must be auto-Exclusive — saw concurrent execution!";
}

// =============================================================================
// Test 2: WorldReadSystem (const World*)  →  stays Parallel  →  MAY overlap
// =============================================================================
TEST(ElysiaSchedulerConcurrent, WorldReadSystemStaysParallel) {
    OverlapDetector det;

    Scheduler scheduler;
    scheduler.system("ReadA")
        .run([&](const World*) { det.enter(); det.leave(); })
        .build();
    scheduler.system("ReadB")
        .run([&](const World*) { det.enter(); det.leave(); })
        .build();

    World world;
    auto exec = ForkUnionExecutor::build_from(scheduler);
    exec->run(&world);

    EXPECT_TRUE(det.saw_overlap.load())
        << "WorldReadSystem should stay Parallel under ForkUnion — "
        << "if this fails occasionally, increase sleep_ms in OverlapDetector";
}

// =============================================================================
// Test 3: Cross-executor result consistency
// =============================================================================
// Two auto-Exclusive systems each increment a counter. The final value must be
// 2 regardless of which executor runs the scheduler.
TEST(ElysiaSchedulerConcurrent, CrossExecutorConsistency) {
    std::atomic<int> counter{0};

    auto build_sched = [&]() {
        Scheduler sched;
        sched.system("IncA")
            .run([&](World*) { counter.fetch_add(1); })
            .build();
        sched.system("IncB")
            .run([&](World*) { counter.fetch_add(1); })
            .build();
        return sched;
    };

    // --- Serial ---
    {
        counter.store(0);
        World w;
        auto s = build_sched();
        SerialExecutor::build_from(s)->run(&w);
        EXPECT_EQ(counter.load(), 2) << "SerialExecutor";
    }

    // --- Taskflow ---
    {
        counter.store(0);
        World w;
        auto s = build_sched();
        TaskflowExecutor::build_from(s)->run(&w);
        EXPECT_EQ(counter.load(), 2) << "TaskflowExecutor";
    }

    // --- ForkUnion ---
    {
        counter.store(0);
        World w;
        auto s = build_sched();
        ForkUnionExecutor::build_from(s)->run(&w);
        EXPECT_EQ(counter.load(), 2) << "ForkUnionExecutor";
    }
}

// =============================================================================
// Test 4: SerialExecutor — WorldMutSystem should also work correctly
// =============================================================================
TEST(ElysiaSchedulerConcurrent, SerialExecutorWorldMut) {
    std::atomic<int> order{0};

    Scheduler scheduler;
    scheduler.system("First")
        .run([&](World*) {
            order.store(1);
        })
        .build();
    scheduler.system("Second")
        .run([&](World*) {
            // If First already ran, order is 1; otherwise 0.
            int seen = order.load();
            order.store(seen == 1 ? 3 : -1);
        })
        .build();
    scheduler.add_dependency("Second", "First");

    World world;
    SerialExecutor::build_from(scheduler)->run(&world);
    EXPECT_EQ(order.load(), 3) << "Dependency should ensure First runs before Second";
}
