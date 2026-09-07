#include <gtest/gtest.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>

import elysia;
import elysia.query;

using namespace elysia;

// --- Test Components ---
struct SharedRes { 
    std::atomic<int> counter; 
    SharedRes() : counter(0) {}
    SharedRes(int v) : counter(v) {}
    SharedRes(SharedRes&& other) noexcept : counter(other.counter.load()) {}
};

struct heavy_work {
    static void run(int ms) {
        auto start = std::chrono::high_resolution_clock::now();
        while(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count() < ms);
    }
};

/**
 * @brief Complex DAG Test
 * Topology:
 *   [Init1 (10ms), Init2 (10ms)] --- Parallel
 *              |
 *   [Proc1 (20ms) -> Proc2 (20ms)] --- Serial Chain
 *              |
 *   [Proc3 (40ms)] --- Parallel to the Proc1->Proc2 chain
 *              |
 *   [Finalize]
 */
void build_complex_app(App& app) {
    app.world().resources().add(SharedRes{});

    // Layer 1: Parallel Init
    app.system("Init1").run([](Res<SharedRes> r) { 
        heavy_work::run(10); r->counter++; 
    }).build();

    app.system("Init2").run([](Res<SharedRes> r) { 
        heavy_work::run(10); r->counter++; 
    }).build();

    // Grouping into Set
    auto init_set = app.scheduler().resolve("InitSet");
    app.scheduler().meta_world().entity(app.scheduler().resolve("Init1")).add(schedule::InSet{init_set});
    app.scheduler().meta_world().entity(app.scheduler().resolve("Init2")).add(schedule::InSet{init_set});

    // Layer 2: Complex Processing
    app.system("Proc1").after("InitSet").run([](Res<SharedRes> r) { 
        heavy_work::run(20); r->counter++; 
    }).build();

    app.system("Proc2").after("Proc1").run([](Res<SharedRes> r) { 
        heavy_work::run(20); r->counter++; 
    }).build();

    app.system("Proc3").after("InitSet").run([](Res<SharedRes> r) { 
        heavy_work::run(40); r->counter++; 
    }).build();

    // Layer 3: Finalize
    app.system("Final").after("Proc2").after("Proc3").run([](Res<SharedRes> r) { 
        r->counter++; 
    }).build();
}

TEST(SchedulerStressTest, SerialExecution) {
    App app;
    build_complex_app(app);
    app.init_serial();

    auto start = std::chrono::high_resolution_clock::now();
    app.update();
    auto end = std::chrono::high_resolution_clock::now();

    EXPECT_EQ(app.world().resources().get<SharedRes>()->counter, 6);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Serial Update took: " << ms << " ms (Expected ~100ms)" << std::endl;
    EXPECT_GE(ms, 100);
}

TEST(SchedulerStressTest, ParallelTaskflowExecution) {
    App app;
    build_complex_app(app);
    app.init_parallel();

    auto start = std::chrono::high_resolution_clock::now();
    app.update();
    auto end = std::chrono::high_resolution_clock::now();

    EXPECT_EQ(app.world().resources().get<SharedRes>()->counter, 6);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Parallel (Taskflow) Update took: " << ms << " ms (Expected ~50-60ms)" << std::endl;
    
    // Efficiency: Parallel Init(10) + Max(Proc1+2 (40), Proc3 (40)) + Final(0) = 50ms
    EXPECT_LT(ms, 80); 
}

TEST(SchedulerStressTest, ExceptionHandling) {
    App app;
    app.scheduler().system("Normal").run([]() { /* fine */ }).build();
    app.scheduler().system("Crasher").run([]() { 
        throw std::runtime_error("System Crash Test!"); 
    }).build();

    app.init_serial();

    EXPECT_THROW({
        app.update();
    }, std::runtime_error);
}

namespace {
struct ExceptionValue { int value; };
struct MissingSystemTarget { Entity entity; };
template<class Executor> class SchedulerExceptions : public ::testing::Test {};
using ExceptionExecutors = ::testing::Types<SerialExecutor, TaskflowExecutor, ForkUnionExecutor>;
TYPED_TEST_SUITE(SchedulerExceptions, ExceptionExecutors);

TYPED_TEST(SchedulerExceptions, PropagatesMissingEntityAndDiscardsPendingCommands) {
    World world;
    Entity target = world.spawn().add(ExceptionValue{7}).entity;
    auto survivor = world.spawn().add(ExceptionValue{0}).entity;
    bool despawn_once = true;
    int downstream = 0;
    Scheduler scheduler;
    scheduler.system("Despawn").run([&](World* w) {
        if (despawn_once) { w->despawn(target); despawn_once = false; }
    }).build();
    scheduler.system("Lookup").after("Despawn").run([&](WorldView w, CommandBuffer* cmd) {
        if (!w.raw()->get_component<ExceptionValue>(target)) {
            cmd->insert(survivor, ExceptionValue{99});
            throw MissingSystemTarget{target};
        }
    }).build();
    scheduler.system("After").after("Lookup").run([&]() { ++downstream; }).build();
    auto exec = TypeParam::build_from(scheduler);
    try {
        exec->run(&world);
        FAIL() << "Missing target must propagate to the caller";
    } catch (const MissingSystemTarget& error) {
        EXPECT_EQ(error.entity, target);
    }
    EXPECT_EQ(downstream, 0);
    EXPECT_EQ(world.get_component<ExceptionValue>(target), nullptr);
    EXPECT_EQ(world.get_component<ExceptionValue>(survivor)->value, 0);
    // Explicit application repair, followed by a new run; no automatic retry.
    target = world.spawn().add(ExceptionValue{7}).entity;
    EXPECT_NO_THROW(exec->run(&world));
    EXPECT_EQ(downstream, 1);
    EXPECT_EQ(world.get_component<ExceptionValue>(survivor)->value, 0);
}

TYPED_TEST(SchedulerExceptions, ExclusiveFailureClearsOtherSystemsBuffers) {
    World world;
    auto entity = world.spawn().add(ExceptionValue{0}).entity;
    bool fail = true;
    int downstream = 0;
    Scheduler scheduler;
    scheduler.system("Queue").run([&](WorldView, CommandBuffer* cmd) {
        if (fail) cmd->insert(entity, ExceptionValue{42});
    }).build();
    scheduler.system("Throw").after("Queue").run([&](World*) { if (fail) throw 17; }).build();
    scheduler.system("After").after("Throw").run([&]() { ++downstream; }).build();
    auto exec = TypeParam::build_from(scheduler);
    try { exec->run(&world); FAIL() << "Expected integer exception"; }
    catch (int value) { EXPECT_EQ(value, 17); }
    EXPECT_EQ(downstream, 0);
    EXPECT_EQ(world.get_component<ExceptionValue>(entity)->value, 0);
    fail = false;
    EXPECT_NO_THROW(exec->run(&world));
    EXPECT_EQ(downstream, 1);
    EXPECT_EQ(world.get_component<ExceptionValue>(entity)->value, 0);
}

template<class Executor> class ParallelSchedulerExceptions : public ::testing::Test {};
using ParallelExceptionExecutors = ::testing::Types<TaskflowExecutor, ForkUnionExecutor>;
TYPED_TEST_SUITE(ParallelSchedulerExceptions, ParallelExceptionExecutors);

TYPED_TEST(ParallelSchedulerExceptions, WaitsForRunningWorkersBeforeRethrowing) {
    if (std::thread::hardware_concurrency() < 2) GTEST_SKIP() << "Requires two workers";
    World world;
    std::atomic<bool> started{false}, release{false}, finished{false};
    int downstream = 0;
    Scheduler scheduler;
    scheduler.system("Running").run([&]() {
        started = true;
        while (!release.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        finished = true;
    }).build();
    scheduler.system("Throw").run([&]() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!started.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        release = true;
        throw std::runtime_error("parallel failure");
    }).build();
    scheduler.system("After").after("Throw").run([&]() { ++downstream; }).build();
    auto exec = TypeParam::build_from(scheduler);
    EXPECT_THROW(exec->run(&world), std::runtime_error);
    EXPECT_TRUE(started.load());
    EXPECT_TRUE(finished.load());
    EXPECT_EQ(downstream, 0);
}
TYPED_TEST(ParallelSchedulerExceptions, SimultaneousFailuresPreserveOneOriginalException) {
    if (std::thread::hardware_concurrency() < 2) GTEST_SKIP() << "Requires two workers";
    World world;
    std::atomic<int> entered{0};
    Scheduler scheduler;
    for (int id = 0; id < 2; ++id) {
        scheduler.system(std::to_string(id)).run([&, id]() {
            entered.fetch_add(1);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (entered.load() < 2 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            throw id;
        }).build();
    }
    auto exec = TypeParam::build_from(scheduler);
    for (int attempt = 0; attempt < 5; ++attempt) {
        entered = 0;
        try { exec->run(&world); FAIL() << "Expected a worker exception"; }
        catch (int id) { EXPECT_TRUE(id == 0 || id == 1); }
        EXPECT_EQ(entered.load(), 2);
    }
}
} // namespace
