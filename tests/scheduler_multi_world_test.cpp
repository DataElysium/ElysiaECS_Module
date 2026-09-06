#include <gtest/gtest.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <future>
#include <barrier>

import elysia.world;
import elysia.schedule;
import elysia.entity;
import elysia.query;

using namespace elysia;

struct Score { int value = 0; };

TEST(ElysiaScheduler, MultiWorldIsolation) {
    // 1. Define a REUSABLE system descriptor (Blueprint)
    // Note: It's NOT built yet, just defined.
    struct ScoreSystem {
        Query<Score> query;
        void operator()(WorldView world) {
            world.update_query(query);
            query.each(world.raw(), [](Score& s) { s.value += 10; });
        }
    };
    auto score_sys = schedule::SystemBuilder("ScoreSys").run<ScoreSystem>();

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

    struct TagSystem {
        Query<Entity, Score> query;
        void operator()(WorldView world, CommandBuffer* cmd) {
            world.update_query(query);
            query.each(world.raw(), [&](Entity e, Score& s) {
                if (s.value > 50) cmd->insert(e, TagB{});
                else cmd->insert(e, TagA{});
            });
        }
    };
    auto spawn_sys = schedule::SystemBuilder("SpawnSys").run<TagSystem>();

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

namespace {
struct DeferredScore { int value; };
struct InitCount { int value = 0; };
struct StatefulScoreSystem {
    Query<Score> query;
    int calls = 0;
    void init(World* world) {
        world->query<InitCount>().each([](InitCount& count) { ++count.value; });
    }
    void operator()(WorldView world) {
        world.update_query(query);
        ++calls;
        query.each(world.raw(), [&](Score& score) { score.value += calls; });
    }
};

template<class Executor> class SchedulerLifetime : public ::testing::Test {};
using ExecutorTypes = ::testing::Types<SerialExecutor, TaskflowExecutor, ForkUnionExecutor>;
TYPED_TEST_SUITE(SchedulerLifetime, ExecutorTypes);

TYPED_TEST(SchedulerLifetime, RejectsWorldSwitchBeforeDispatch) {
    World a, b;
    auto ea = a.spawn().add(Score{0}).entity;
    auto eb = b.spawn().add(Score{100}).entity;
    Scheduler scheduler;
    scheduler.system("Deferred").run([](CommandBuffer& cmd, Score& score, Entity e) {
        ++score.value;
        cmd.insert(e, DeferredScore{score.value});
    }).build();
    auto exec = TypeParam::build_from(scheduler);
    exec->run(&a);
    EXPECT_THROW(exec->run(&b), std::logic_error);
    EXPECT_EQ(a.get_component<Score>(ea)->value, 1);
    EXPECT_EQ(b.get_component<Score>(eb)->value, 100);
    EXPECT_EQ(b.get_component<DeferredScore>(eb), nullptr);
    EXPECT_THROW(exec->init_all(&b), std::logic_error);
    auto other = TypeParam::build_from(scheduler);
    EXPECT_NO_THROW(other->run(&b));
    EXPECT_EQ(b.get_component<Score>(eb)->value, 101);
    exec->run(&a);
    EXPECT_EQ(a.get_component<Score>(ea)->value, 2);
    ASSERT_NE(a.get_component<DeferredScore>(ea), nullptr);
    EXPECT_EQ(a.get_component<DeferredScore>(ea)->value, 2);
}

TYPED_TEST(SchedulerLifetime, OneDefinitionCreatesIndependentWorldExecutors) {
    World a, b;
    auto ea = a.spawn().add(Score{0}).entity;
    auto eb = b.spawn().add(Score{100}).entity;
    Scheduler scheduler;
    scheduler.system("Counter").run([calls = 0](Score& s) mutable { s.value += ++calls; }).build();
    auto first = TypeParam::build_from(scheduler);
    first->run(&a);
    auto second = TypeParam::build_from(scheduler);
    EXPECT_NO_THROW(second->run(&b));
    first->run(&a);
    EXPECT_EQ(a.get_component<Score>(ea)->value, 3);
    EXPECT_EQ(b.get_component<Score>(eb)->value, 101);
}

TYPED_TEST(SchedulerLifetime, PreservesFunctorAcrossExecutorsAndInitializations) {
    World a, b;
    auto ea = a.spawn().add(Score{0}).add(InitCount{}).entity;
    auto eb = b.spawn().add(Score{100}).add(InitCount{}).entity;
    Scheduler first, second;
    auto definition = schedule::SystemBuilder("Stateful").run<StatefulScoreSystem>();
    definition.build(first);
    definition.build(second);
    auto runtime = first.instantiate(a);
    auto exec = TypeParam::build_from(runtime);
    exec->run(&a);
    exec->init_all(&a);
    auto replacement = TypeParam::build_from(runtime);
    replacement->run(&a);
    EXPECT_EQ(a.get_component<Score>(ea)->value, 3);
    EXPECT_EQ(a.get_component<InitCount>(ea)->value, 1);
    EXPECT_THROW(replacement->run(&b), std::logic_error);
    EXPECT_EQ(b.get_component<InitCount>(eb)->value, 0);
    EXPECT_EQ(b.get_component<Score>(eb)->value, 100);
    TypeParam::build_from(second)->run(&b);
    EXPECT_EQ(b.get_component<Score>(eb)->value, 101);
    EXPECT_EQ(b.get_component<InitCount>(eb)->value, 1);
    exec->run(&a);
    EXPECT_EQ(a.get_component<Score>(ea)->value, 6);
}

TYPED_TEST(SchedulerLifetime, RuntimeSnapshotsRunConcurrentlyWithoutScheduler) {
    World a, b;
    auto ea = a.spawn().add(Score{0}).add(InitCount{}).entity;
    auto eb = b.spawn().add(Score{100}).add(InitCount{}).entity;
    // B has a different archetype layout, exercising independent query caches.
    b.spawn().add(DeferredScore{-1});
    std::shared_ptr<ScheduleRuntime> ra, rb;
    std::barrier start(2);
    {
        Scheduler scheduler;
        scheduler.system("Rendezvous").run([&start](World*) { start.arrive_and_wait(); }).build();
        scheduler.system("Stateful").after("Rendezvous").run<StatefulScoreSystem>().build();
        scheduler.system("Deferred").after("Stateful").run(
            [calls = 0](CommandBuffer& cmd, Score& score, Entity e) mutable {
                score.value += ++calls;
                cmd.insert(e, DeferredScore{score.value});
            }).build();
        scheduler.system("Flush").after("Deferred").kind(schedule::SpecialSystemKind::ApplyDeferred).build();
        scheduler.system("Check").after("Flush").run([](Score& score, const DeferredScore& deferred) {
            EXPECT_EQ(score.value, deferred.value);
        }).build();
        ra = scheduler.instantiate(a);
        rb = scheduler.instantiate(b);
        // Later edits must not change either existing runtime.
        scheduler.system("Late").run([](Score& score) { score.value = -1000; }).build();
    }
    auto first = TypeParam::build_from(ra);
    auto second = TypeParam::build_from(rb);
    auto run = [](auto& exec, World& world) { for (int i = 0; i < 20; ++i) exec->run(&world); };
    auto work_a = std::async(std::launch::async, [&] { run(first, a); });
    auto work_b = std::async(std::launch::async, [&] { run(second, b); });
    work_a.get();
    work_b.get();
    EXPECT_EQ(a.get_component<Score>(ea)->value, 420);
    EXPECT_EQ(b.get_component<Score>(eb)->value, 520);
    EXPECT_EQ(a.get_component<DeferredScore>(ea)->value, 420);
    EXPECT_EQ(b.get_component<DeferredScore>(eb)->value, 520);
    EXPECT_EQ(a.get_component<InitCount>(ea)->value, 1);
    EXPECT_EQ(b.get_component<InitCount>(eb)->value, 1);
}

TEST(ElysiaScheduler, RuntimeBindingIsSharedAcrossBackends) {
    World a, b;
    auto ea = a.spawn().add(Score{0}).add(InitCount{}).entity;
    Scheduler scheduler;
    scheduler.system("Stateful").run<StatefulScoreSystem>().build();
    auto runtime = scheduler.instantiate(a);
    SerialExecutor::build_from(runtime)->run(&a);
    auto taskflow = TaskflowExecutor::build_from(runtime);
    EXPECT_THROW(taskflow->run(&b), std::logic_error);
    taskflow->run(&a);
    auto fork = ForkUnionExecutor::build_from(runtime);
    EXPECT_THROW(fork->run(&b), std::logic_error);
    fork->run(&a);
    EXPECT_EQ(a.get_component<Score>(ea)->value, 6);
    EXPECT_EQ(a.get_component<InitCount>(ea)->value, 1);
}
} // namespace
