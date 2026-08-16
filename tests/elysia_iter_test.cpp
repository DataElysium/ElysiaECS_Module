#include <gtest/gtest.h>
#include <vector>

import elysia;

using namespace elysia;

struct IterCompA { int value; };
struct IterCompB { int value; };

TEST(ElysiaIter, BasicExecution) {
    App app;
    bool executed = false;

    app.scheduler().system("IterSystem").run([&](Iter& it) {
        executed = true;
        const World& w = it.world();
        (void)w;
    }).build(app.scheduler());

    app.update();
    EXPECT_TRUE(executed);
}

TEST(ElysiaIter, CommandsAccess) {
    App app;
    bool spawned = false;

    // We need an entity ID that the CommandBuffer can work with safely.
    // In a real system, you'd usually get this from world.spawn() before the loop
    // or use a smarter CommandBuffer API.
    Entity test_e = {1, 1}; // Use a low ID for spawn_at

    app.scheduler().system("CommandSystem").run([&](Iter& it) {
        it.commands().spawn(test_e);
        it.commands().insert(test_e, IterCompA{42});
    }).build(app.scheduler());

    app.update(); 
    
    auto q = app.world().query<IterCompA>();
    q.each([&](IterCompA& a) {
        if (a.value == 42) spawned = true;
    });
    
    EXPECT_TRUE(spawned);
}

TEST(ElysiaIter, QueryInsideIter) {
    App app;
    int count = 0;

    app.world().spawn().add(IterCompB{10});
    app.world().spawn().add(IterCompB{20});

    app.scheduler().system("QuerySystem").run([&](Iter& it) {
        it.query<IterCompB>().each([&](IterCompB& b) {
            count += b.value;
        });
    }).build(app.scheduler());

    app.update();
    EXPECT_EQ(count, 30);
}

TEST(ElysiaIter, ReadOnlySafety) {
    App app;
    app.scheduler().system("SafetySystem").run([](Iter& it) {
        auto& w = it.world();
        (void)w;
    }).build(app.scheduler());
    app.update();
}
