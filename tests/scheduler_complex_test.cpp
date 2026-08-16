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
