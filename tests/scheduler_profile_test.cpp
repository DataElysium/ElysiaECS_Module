#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
import elysia.app;
import elysia.entity;
import elysia.observer;
import elysia.world;
import elysia.schedule;
namespace {
using namespace elysia;
const SystemTiming& named(const ExecutionProfile& profile, const std::string& name) {
    for (auto& sample : profile.systems) if (sample.name == name) return sample;
    throw std::runtime_error("Missing profile entry: " + name);
}
template<class Executor> class ExecutionProfiling : public testing::Test {};
using Executors = testing::Types<SerialExecutor, TaskflowExecutor>;
TYPED_TEST_SUITE(ExecutionProfiling, Executors);

TYPED_TEST(ExecutionProfiling, FailureFlushRetryAndOwnedSnapshot) {
    World world;
    Scheduler scheduler;
    bool fail = true;
    scheduler.system("empty callable").run([] {}).build();
    scheduler.system("failure").after("empty callable").run([&] {
        if (fail) throw std::runtime_error("expected");
    }).build();
    scheduler.system("flush").after("failure")
        .kind(schedule::SpecialSystemKind::ApplyDeferred).build();
    auto executor = TypeParam::build_from(scheduler);
    EXPECT_THROW(executor->run(&world), std::runtime_error);
    const auto failed = executor->profile();
#ifdef ELYSIA_PERF_OVERLAY
    ASSERT_TRUE(failed.enabled);
    EXPECT_FALSE(failed.completed);
    EXPECT_TRUE(named(failed, "empty callable").executed);
    EXPECT_TRUE(named(failed, "failure").failed);
    EXPECT_FALSE(named(failed, "flush").executed);
    EXPECT_FALSE(named(failed, "[final flush]").executed);
    EXPECT_GE(failed.wall_ms, named(failed, "failure").ms);
#else
    EXPECT_FALSE(failed.enabled);
    EXPECT_TRUE(failed.systems.empty());
#endif
    fail = false;
    executor->run(&world);
    const auto success = executor->profile();
    executor.reset();
#ifdef ELYSIA_PERF_OVERLAY
    EXPECT_TRUE(success.completed);
    EXPECT_FALSE(named(success, "failure").failed);
    EXPECT_FALSE(named(success, "[initialize]").executed);
    EXPECT_EQ(named(success, "flush").kind, TimingKind::ApplyDeferred);
    EXPECT_TRUE(named(success, "flush").executed);
    EXPECT_TRUE(named(success, "[final flush]").executed);
    EXPECT_TRUE(named(failed, "failure").failed); // previous copy is immutable
#endif
}

TYPED_TEST(ExecutionProfiling, IndependentParallelSlotsAndEmptySchedule) {
    World world;
    Scheduler empty;
    auto noop = TypeParam::build_from(empty);
    noop->run(&world);
#ifdef ELYSIA_PERF_OVERLAY
    EXPECT_TRUE(noop->profile().completed);
    ASSERT_EQ(noop->profile().systems.size(), 2);
    EXPECT_TRUE(named(noop->profile(), "[final flush]").executed);
#endif
    Scheduler scheduler;
    for (int i = 0; i < 64; ++i)
        scheduler.system("work " + std::to_string(i)).run([] {}).build();
    auto executor = TypeParam::build_from(scheduler);
    for (int n = 0; n < 5; ++n) {
        executor->run(&world);
#ifdef ELYSIA_PERF_OVERLAY
        for (int i = 0; i < 64; ++i) {
            const auto& sample = named(executor->profile(), "work " + std::to_string(i));
            EXPECT_TRUE(sample.executed);
            EXPECT_FALSE(sample.failed);
        }
#endif
    }
}
TEST(AppProfiling, ExecutorSwitchPreservesNames) {
    App app;
    EXPECT_TRUE(app.execution_profile().systems.empty());
    app.system("step").run([] {}).build();
    app.init_serial();
    app.update();
    const auto serial = app.execution_profile();
    app.init_parallel();
    app.update();
    const auto parallel = app.execution_profile();
#ifdef ELYSIA_PERF_OVERLAY
    EXPECT_TRUE(named(serial, "step").executed);
    EXPECT_TRUE(named(parallel, "step").executed);
#else
    EXPECT_FALSE(serial.enabled);
    EXPECT_FALSE(parallel.enabled);
#endif
}

struct ProfileComponent { int value = 0; };
TYPED_TEST(ExecutionProfiling, FlushFailureIsAttributedAndDoesNotLeaveStaleSamples) {
    World world;
    auto entity = world.spawn().entity;
    world.observer().on_add<ProfileComponent>([](Entity) { throw std::runtime_error("observer failed"); });
    Scheduler scheduler;
    scheduler.system("enqueue").run([entity](CommandBuffer& cmd) {
        cmd.insert(entity, ProfileComponent{1});
    }).build();
    scheduler.system("flush").after("enqueue")
        .kind(schedule::SpecialSystemKind::ApplyDeferred).build();
    auto executor = TypeParam::build_from(scheduler);
    EXPECT_THROW(executor->run(&world), std::runtime_error);
#ifdef ELYSIA_PERF_OVERLAY
    const auto& profile = executor->profile();
    EXPECT_FALSE(profile.completed);
    EXPECT_TRUE(named(profile, "enqueue").executed);
    EXPECT_TRUE(named(profile, "flush").failed);
    EXPECT_FALSE(named(profile, "[final flush]").executed);
#endif
}
} // namespace
