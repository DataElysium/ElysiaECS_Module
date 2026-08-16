#include <gtest/gtest.h>
#include <string>
#include <vector>

import elysia.world;
import elysia.schedule;
import elysia.entity;
import elysia.meta;

using namespace elysia;

struct Counter { int value = 0; };

TEST(ElysiaScheduler, MetaWorldExecution) {
    // 1. The Game World (Data)
    World game_world;
    auto e = game_world.spawn().add(Counter{0}).entity;

    // 2. The Scheduler (Logic)
    Scheduler scheduler;

    // 3. Define System A: Increment Counter
    scheduler.add_system("Increment", [](World* w) {
        // This lambda runs inside the "Meta World" context, 
        // but 'w' is the "Game World" passed to run().
        w->query<Counter>().each([](Counter& c) {
            c.value++;
        });
    });

    // 4. Run Scheduler
    auto exec = SerialExecutor::build_from(scheduler);
    exec->run(&game_world);

    // 5. Verify Side Effects
    auto* c = game_world.get_component<Counter>(e);
    EXPECT_EQ(c->value, 1);

    // Run again
    exec->run(&game_world);
    EXPECT_EQ(c->value, 2);
}

TEST(ElysiaScheduler, DependencyStructure) {
    World game_world;
    auto e = game_world.spawn().add(Counter{0}).entity;

    Scheduler scheduler;
    
    // SysA: +1
    scheduler.add_system("SysA", [](World* w) {
        w->query<Counter>().each([](Counter& c) { c.value += 1; });
    });
    
    // SysB: *2
    scheduler.add_system("SysB", [](World* w) {
        w->query<Counter>().each([](Counter& c) { c.value *= 2; });
    });
    
    // Dependency: B runs AFTER A (A -> B)
    scheduler.add_dependency("SysB", "SysA"); 
    
    // Run
    SerialExecutor::build_from(scheduler)->run(&game_world);
    
    // Verify
    auto* c = game_world.get_component<Counter>(e);
    // Expected: (0 + 1) * 2 = 2
    // If order was wrong: (0 * 2) + 1 = 1
    EXPECT_EQ(c->value, 2);
}

TEST(ElysiaScheduler, MultiDependency) {
    World game_world;
    std::string order;
    
    // Shared Resource to track execution order
    struct OrderTracker { std::string* str; };
    game_world.resources().add(OrderTracker{&order});

    Scheduler scheduler;
    
    scheduler.add_system("SysA", [](World* w) {
        if(auto* t = w->get_resource<OrderTracker>()) *(t->str) += "A";
    });
    
    scheduler.add_system("SysB", [](World* w) {
        if(auto* t = w->get_resource<OrderTracker>()) *(t->str) += "B";
    });
    
    scheduler.add_system("SysC", [](World* w) {
        if(auto* t = w->get_resource<OrderTracker>()) *(t->str) += "C";
    });
    
    // Dependency: B runs AFTER A, C runs AFTER A
    scheduler.add_dependency("SysB", "SysA");
    scheduler.add_dependency("SysC", "SysA");
    
    SerialExecutor::build_from(scheduler)->run(&game_world);
    
    // A must be first. B and C order is undefined (could be ABC or ACB).
    EXPECT_EQ(order.length(), 3);
    EXPECT_EQ(order[0], 'A');
    EXPECT_TRUE(order == "ABC" || order == "ACB");
}
