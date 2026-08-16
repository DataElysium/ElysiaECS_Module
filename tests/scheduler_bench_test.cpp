#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <vector>
#include <atomic>
#include <memory>

import elysia.world;
import elysia.schedule;
import elysia.entity;
import elysia.meta;

using namespace elysia;

struct Pos { float x, y; };
struct Vel { float dx, dy; };

struct BenchData {
    std::shared_ptr<std::atomic<int>> counter = std::make_shared<std::atomic<int>>(0);
};

TEST(ElysiaBench, ExtremeParallelScale) {
    const int sys_count = 10000;
    
    auto run_extreme_bench = [&](const char* name, auto executor_factory) {
        World world;
        world.resources().add(BenchData{});
        world.spawn(); 

        Scheduler scheduler;
        
        // Define 10,000 parallel systems
        for(int i = 0; i < sys_count; ++i) {
            std::string sys_name = "Sys_" + std::to_string(i);
            scheduler.system(sys_name).run([](Res<BenchData> data, Entity) {
                data->counter->fetch_add(1, std::memory_order_relaxed);
            }).build();
        }

        auto exec = executor_factory(scheduler);
        
        auto start_run = std::chrono::high_resolution_clock::now();
        exec->run(&world);
        auto end_run = std::chrono::high_resolution_clock::now();
        
        auto run_time = std::chrono::duration_cast<std::chrono::microseconds>(end_run - start_run);
        std::cout << "[Bench] " << name << " Execution (10k tasks): " << run_time.count() << " us" << std::endl;

        EXPECT_EQ(world.get_resource<BenchData>()->counter->load(), sys_count);
    };

    std::cout << "--- Extreme Parallel Scale Test (10,000 Systems) ---" << std::endl;
    run_extreme_bench("Taskflow", [](Scheduler& s) { return TaskflowExecutor::build_from(s); });
    run_extreme_bench("ForkUnion", [](Scheduler& s) { return ForkUnionExecutor::build_from(s); });
}

TEST(ElysiaBench, GrandmaFriendlyAPI) {
    World world;
    for(int i=0; i<100; ++i) world.spawn().add(Pos{0,0});

    Scheduler scheduler;

    // The "Age Progresses" Flow
    scheduler.chain("First", schedule::Sync, "Update", schedule::Sync, "Last");

    auto update = scheduler.phase("Update");
    update.add("Move", [](Pos& p){ p.x += 1.0f; });
    update.add("Jump", [](Pos& p){ p.y += 5.0f; });

    auto exec = ForkUnionExecutor::build_from(scheduler);
    
    // Run!
    exec->run(&world);

    // Verify
    float sum_x = 0;
    world.query<Pos>().each([&](Pos& p){ sum_x += p.x; });
    EXPECT_EQ(sum_x, 100.0f);
}
