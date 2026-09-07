#include <gtest/gtest.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <type_traits>

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

namespace {
struct AffinityRecord { std::thread::id expected; int calls = 0; };
struct CallerFunctor {
    void operator()(WorldView w) {
        auto* record = w.resources().get<AffinityRecord>();
        EXPECT_EQ(std::this_thread::get_id(), record->expected);
        ++record->calls;
    }
};
struct RequiredSettings { int value = 7; };
struct DiagnosticPosition { int value = 0; };
template<class Executor> class CallerAndDiagnostics : public ::testing::Test {};
using CallerExecutors = ::testing::Types<SerialExecutor, TaskflowExecutor, ForkUnionExecutor>;
TYPED_TEST_SUITE(CallerAndDiagnostics, CallerExecutors);

TYPED_TEST(CallerAndDiagnostics, CallerAffinityPreservesDependenciesAndFollowsRunThread) {
    World world;
    world.resources().add(AffinityRecord{std::this_thread::get_id()});
    Scheduler scheduler;
    std::atomic<int> finished{0};
    scheduler.system("Begin").on_caller_thread().run<CallerFunctor>().build();
    for (int i = 0; i < 8; ++i) {
        auto name = "Worker" + std::to_string(i);
        scheduler.system(name).after("Begin").before("Join").run([&] {
            if constexpr (std::is_same_v<TypeParam, TaskflowExecutor>)
                EXPECT_NE(std::this_thread::get_id(), world.resources().get<AffinityRecord>()->expected);
            ++finished;
        }).build();
    }
    scheduler.system("Join").on_caller_thread().run([&] {
        EXPECT_EQ(finished.load(), 8);
        EXPECT_EQ(std::this_thread::get_id(), world.resources().get<AffinityRecord>()->expected);
    }).build();
    scheduler.system("AdjacentCaller").after("Join").run<CallerFunctor>().on_caller_thread().build();
    scheduler.system("End").after("AdjacentCaller").on_caller_thread().run<CallerFunctor>().build();
    auto graph = schedule::to_mermaid(scheduler.meta_world());
    EXPECT_NE(graph.find("[caller thread]"), std::string::npos);
    auto exec = TypeParam::build_from(scheduler);
    exec->run(&world);
    EXPECT_EQ(world.resources().get<AffinityRecord>()->calls, 3);
    finished = 0;
    std::thread another_caller([&] {
        world.resources().get<AffinityRecord>()->expected = std::this_thread::get_id();
        exec->run(&world);
    });
    another_caller.join();
    EXPECT_EQ(world.resources().get<AffinityRecord>()->calls, 6);
}

TYPED_TEST(CallerAndDiagnostics, CallerFailureSkipsSuccessorsAndCanRunAgain) {
    World world;
    auto survivor = world.spawn().add(ExceptionValue{4}).entity;
    Scheduler scheduler;
    bool fail = true;
    int downstream = 0;
    scheduler.system("CallerFailure").on_caller_thread().run([&](WorldView, CommandBuffer* cmd) {
        cmd->insert(survivor, ExceptionValue{99});
        if (fail) throw 73;
    }).build();
    scheduler.system("Downstream").after("CallerFailure").run([&] { ++downstream; }).build();
    auto exec = TypeParam::build_from(scheduler);
    try { exec->run(&world); FAIL() << "Expected caller failure"; }
    catch (int value) { EXPECT_EQ(value, 73); }
    EXPECT_EQ(downstream, 0);
    EXPECT_EQ(world.get_component<ExceptionValue>(survivor)->value, 4);
    fail = false;
    exec->run(&world);
    EXPECT_EQ(downstream, 1);
    EXPECT_EQ(world.get_component<ExceptionValue>(survivor)->value, 99);
}

TYPED_TEST(CallerAndDiagnostics, MissingResNamesResourceAndSystemForBothAdapters) {
    for (bool each : {false, true}) {
        World world;
        world.spawn().add(DiagnosticPosition{});
        Scheduler scheduler;
        int calls = 0;
        if (each) scheduler.system("NeedsSettings").run([&](DiagnosticPosition&, Res<RequiredSettings>) { ++calls; }).build();
        else scheduler.system("NeedsSettings").run([&](Res<RequiredSettings>) { ++calls; }).build();
        auto exec = TypeParam::build_from(scheduler);
        try { exec->run(&world); FAIL() << "Expected missing resource"; }
        catch (const MissingResourceError& error) {
            EXPECT_EQ(error.system_name, "NeedsSettings");
            EXPECT_NE(error.resource_name.find("RequiredSettings"), std::string::npos);
            EXPECT_NE(std::string(error.what()).find("NeedsSettings"), std::string::npos);
        }
        EXPECT_EQ(calls, 0);
        world.resources().add(RequiredSettings{});
        exec->run(&world);
        EXPECT_EQ(calls, 1);
    }
}

TEST(ScheduleDiagnosticDetails, MissingLabelsAndCyclesIdentifyConnections) {
    Scheduler missing;
    missing.system("Weapons.Fire").after("Misspelled.Target").run([] {}).build();
    try { schedule::compile_dag(missing.meta_world(), {schedule::MissingLabelPolicy::Reject}); FAIL(); }
    catch (const std::logic_error& error) {
        EXPECT_NE(std::string(error.what()).find("Weapons.Fire"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Misspelled.Target"), std::string::npos);
    }
    Scheduler cyclic;
    cyclic.system("Alpha").after("Beta").run([] {}).build();
    cyclic.system("Beta").after("Alpha").run([] {}).build();
    try { schedule::compile_dag(cyclic.meta_world()); FAIL(); }
    catch (const std::logic_error& error) {
        EXPECT_NE(std::string(error.what()).find("Alpha"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Beta"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find(" -> "), std::string::npos);
    }
}
} // namespace
