#include <gtest/gtest.h>
#include <string>
#include <atomic>
#include <stdexcept>
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

namespace elysia_test::schedule_mermaid {
TEST(ScheduleMermaid, EmptyAndEscapedLabels) {
    Scheduler scheduler;
    EXPECT_EQ(schedule::to_mermaid(scheduler.meta_world()), "flowchart TD\n");
    scheduler.system("Quote \" <tag> & # ` \\ \nend").run([]{}).build();
    auto text = schedule::to_mermaid(scheduler.meta_world(), schedule::MermaidDirection::LeftToRight);
    EXPECT_TRUE(text.starts_with("flowchart LR\n"));
    EXPECT_NE(text.find("Quote #34; #60;tag#62; #38; #35; #96; #92;  end"), std::string::npos);
    EXPECT_EQ(text.find("<tag>"), std::string::npos);
    EXPECT_EQ(text, schedule::to_mermaid(scheduler.meta_world(), schedule::MermaidDirection::LeftToRight));
}

TEST(ScheduleMermaid, PreservesDiamondAndSetBoundaries) {
    Scheduler scheduler;
    scheduler.system("Input").in_set("Frame").run([]{}).build();
    scheduler.system("Left").in_set("Frame").after("Input").run([]{}).build();
    scheduler.system("Right").in_set("Frame").after("Input").exclusive().run([]{}).build();
    scheduler.system("Join").in_set("Frame").after("Left").after("Right").run([]{}).build();
    scheduler.system("Flush").after("Frame").kind(schedule::SpecialSystemKind::ApplyDeferred).build();
    auto dag = schedule::build_dag(scheduler.meta_world());
    auto text = schedule::to_mermaid(dag, scheduler.meta_world());
    EXPECT_NE(text.find("Frame [set start]"), std::string::npos);
    EXPECT_NE(text.find("Frame [set end]"), std::string::npos);
    EXPECT_NE(text.find("Right [exclusive]"), std::string::npos);
    EXPECT_NE(text.find("Flush [ApplyDeferred]"), std::string::npos);
    size_t expected = 0, actual = 0, pos = 0;
    for (size_t i=0;i<dag.node_count();++i) for (auto& edge:dag.out_edges(i)) {
        ++expected;
        EXPECT_NE(text.find("  n"+std::to_string(i)+" --> n"+std::to_string(edge.to)+"\n"), std::string::npos);
    }
    while ((pos=text.find(" --> ",pos))!=std::string::npos) {++actual;pos+=5;}
    EXPECT_EQ(actual,expected);
}

TEST(ScheduleMermaid, GroupsNestedSetsWithoutChangingNodesOrEdges) {
    Scheduler scheduler;
    scheduler.phase("Frame");
    scheduler.system("Left").in_set("Motion \"drivers\"").run([]{}).build();
    scheduler.system("Right").in_set("Motion \"drivers\"").run([]{}).build();
    scheduler.meta_world().entity(scheduler.resolve("Motion \"drivers\""))
        .add(schedule::InSet{scheduler.resolve("Frame")});
    scheduler.system("Join").after("Frame").run([]{}).build();
    auto dag = schedule::build_dag(scheduler.meta_world());
    auto grouped = schedule::to_mermaid(dag, scheduler.meta_world());
    auto flat = schedule::to_mermaid(dag, scheduler.meta_world(),
                                    schedule::MermaidDirection::TopDown,
                                    schedule::MermaidLayout::Flat);
    auto outer = grouped.find("[\"Frame\"]");
    auto inner = grouped.find("[\"Motion #34;drivers#34;\"]");
    EXPECT_NE(outer, std::string::npos);
    EXPECT_NE(inner, std::string::npos);
    EXPECT_LT(outer, inner);
    EXPECT_EQ(flat.find("subgraph"), std::string::npos);
    // The grouping is presentation only: exactly the same numeric nodes and edges.
    auto lines = [](const std::string& text) {
        std::vector<std::string> out;
        size_t start = 0;
        while ((start = text.find("  n", start)) != std::string::npos) {
            auto end = text.find('\n', start);
            out.push_back(text.substr(start, end - start));
            start = end;
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    EXPECT_EQ(lines(grouped), lines(flat));
    EXPECT_EQ(grouped, schedule::to_mermaid(dag, scheduler.meta_world()));
    auto compiled = schedule::compile_dag(scheduler.meta_world());
    EXPECT_NO_THROW(schedule::to_mermaid(compiled, scheduler.meta_world()));
    // Malformed hierarchy must remain exportable for debugging.
    scheduler.meta_world().entity(scheduler.resolve("Frame"))
        .add(schedule::InSet{scheduler.resolve("Motion \"drivers\"")});
    EXPECT_NO_THROW(schedule::to_mermaid(scheduler.meta_world()));
}

TEST(ScheduleMermaid, CompactSetsHideOnlyMembershipWiring) {
    Scheduler scheduler;
    scheduler.system("Input").run([]{}).build();
    scheduler.system("Left").in_set("Frame").after("Input").run([]{}).build();
    scheduler.system("Right").in_set("Frame").after("Left").run([]{}).build();
    scheduler.system("Output").after("Frame").run([]{}).build();
    auto dag = schedule::build_dag(scheduler.meta_world());
    auto compact = schedule::to_mermaid(dag, scheduler.meta_world(),
        schedule::MermaidDirection::TopDown, schedule::MermaidLayout::CompactSets);
    auto node = [&](std::string_view name, schedule::GraphNode::Type type) {
        for (size_t i = 0; i < dag.node_count(); ++i)
            if (dag.key(i).type == type &&
                scheduler.meta_world().get_component<schedule::SysName>(dag.key(i).entity)->value == name)
                return "n" + std::to_string(i);
        return std::string{"missing"};
    };
    // These are declared dependencies, including the collective set completion.
    for (const auto& edge : std::vector<std::pair<std::string,std::string>>{
            {node("Input",schedule::GraphNode::System),node("Left",schedule::GraphNode::System)},
            {node("Left",schedule::GraphNode::System),node("Right",schedule::GraphNode::System)},
            {node("Frame",schedule::GraphNode::SetEnd),node("Output",schedule::GraphNode::System)}})
        EXPECT_NE(compact.find(edge.first + " --> " + edge.second),std::string::npos);
    EXPECT_EQ(compact.find("Frame [set start]"),std::string::npos);
    EXPECT_NE(compact.find("Frame [set end]"),std::string::npos);
    size_t arrows=0, pos=0;
    while ((pos=compact.find(" --> ",pos))!=std::string::npos) { ++arrows; pos+=5; }
    EXPECT_EQ(arrows,3u);
    EXPECT_NE(compact.find("subgraph"),std::string::npos);
}

TEST(ScheduleMermaid, UnresolvedLabelsRemainVisibleAndExecuteAsEmptySlots) {
    Scheduler scheduler;
    int ran = 0;
    scheduler.system("Consumer").after("OptionalMod").run([&]{ ++ran; }).build();
    auto dag = schedule::build_dag(scheduler.meta_world());
    EXPECT_EQ(dag.node_count(),2u);
    EXPECT_EQ(schedule::compile_dag(scheduler.meta_world()).node_count(),1u);
    EXPECT_NE(schedule::to_mermaid(dag,scheduler.meta_world()).find("OptionalMod [unresolved]"),std::string::npos);
    World world;
    SerialExecutor::build_from(scheduler)->run(&world);
    TaskflowExecutor::build_from(scheduler)->run(&world);
    ForkUnionExecutor::build_from(scheduler)->run(&world);
    EXPECT_EQ(ran,3);
    // A registered no-op is still a real system and retains the ordering edge.
    scheduler.system("OptionalMod").run([]{}).build();
    auto filled = schedule::build_dag(scheduler.meta_world());
    EXPECT_EQ(filled.node_count(),2u);
    EXPECT_NE(schedule::to_mermaid(filled,scheduler.meta_world()).find("OptionalMod"),std::string::npos);
}

TEST(ScheduleMermaid, ExportDoesNotExecuteAndCanDisplayCycles) {
    Scheduler scheduler;
    int ran=0;
    scheduler.system("A").after("B").run([&]{ ++ran; }).build();
    scheduler.system("B").after("A").run([]{}).build();
    auto text=schedule::to_mermaid(scheduler.meta_world());
    EXPECT_NE(text.find("n0 --> n1"),std::string::npos);
    EXPECT_NE(text.find("n1 --> n0"),std::string::npos);
    EXPECT_EQ(ran,0);
}
} // namespace elysia_test::schedule_mermaid

namespace elysia_test::schedule_compilation {
using namespace elysia;
using schedule::CompileOptions;
using schedule::MissingLabelPolicy;

template<class Executor> void check_snapshot_injection() {
    World old_world, new_world;
    Scheduler scheduler;
    std::string order;
    scheduler.system("A").after("Source").run([&]{order += 'A';}).build();
    scheduler.add_dependency("Middle", "A");
    scheduler.system("B").after("Middle").before("Sink").run([&]{order += 'B';}).build();
    auto diagram = schedule::to_mermaid(scheduler.meta_world());
    auto compiled = schedule::compile_dag(scheduler.meta_world());
    EXPECT_EQ(compiled.node_count(),3u); // A -> empty Middle -> B
    EXPECT_EQ(schedule::to_mermaid(scheduler.meta_world()),diagram);
    EXPECT_THROW(Executor::build_from(scheduler, {MissingLabelPolicy::Reject}), std::logic_error);
    auto old = Executor::build_from(scheduler);
    scheduler.system("Source").run([&]{order += 'S';}).build();
    scheduler.system("Middle").run([&]{order += 'M';}).build();
    scheduler.system("Sink").run([&]{order += 'T';}).build();
    auto fresh = Executor::build_from(scheduler, {MissingLabelPolicy::Reject});
    old->run(&old_world);
    EXPECT_EQ(order,"AB");
    order.clear();
    fresh->run(&new_world);
    EXPECT_EQ(order,"SAMBT");
    order.clear();
    old->run(&old_world);
    EXPECT_EQ(order,"AB");
}

TEST(ScheduleCompilation, AllExecutorsPreserveMiddleSlotsAndSnapshotIsolation) {
    check_snapshot_injection<SerialExecutor>();
    check_snapshot_injection<TaskflowExecutor>();
    check_snapshot_injection<ForkUnionExecutor>();
}

TEST(ScheduleCompilation, EndpointPruningIsRecursiveAndDoesNotMutateMetadata) {
    Scheduler scheduler;
    scheduler.add_dependency("Source1","Source0");
    scheduler.system("Work").after("Source1").before("Sink0").run([]{}).build();
    scheduler.add_dependency("Sink1","Sink0");
    scheduler.resolve("Isolated");
    auto diagram = schedule::to_mermaid(scheduler.meta_world());
    EXPECT_EQ(schedule::build_dag(scheduler.meta_world()).node_count(),6u);
    EXPECT_EQ(schedule::compile_dag(scheduler.meta_world(),{MissingLabelPolicy::Fill,false}).node_count(),6u);
    auto plan = schedule::compile_dag(scheduler.meta_world());
    ASSERT_EQ(plan.node_count(),1u);
    EXPECT_EQ(plan.key(0).entity,scheduler.resolve("Work"));
    EXPECT_EQ(schedule::to_mermaid(scheduler.meta_world()),diagram);
    EXPECT_NE(diagram.find("Source0 [unresolved]"),std::string::npos);
    // Reject happens before pruning: even an isolated missing label is diagnosed.
    EXPECT_THROW(schedule::compile_dag(scheduler.meta_world(),{MissingLabelPolicy::Reject}),std::logic_error);
}

TEST(ScheduleCompilation, UnresolvedCyclesAreRejectedByEveryExecutor) {
    Scheduler scheduler;
    scheduler.add_dependency("X","Y");
    scheduler.add_dependency("Y","X");
    EXPECT_EQ(schedule::build_dag(scheduler.meta_world()).node_count(),2u);
    EXPECT_NO_THROW(schedule::to_mermaid(scheduler.meta_world()));
    EXPECT_THROW(SerialExecutor::build_from(scheduler),std::logic_error);
    EXPECT_THROW(TaskflowExecutor::build_from(scheduler),std::logic_error);
    EXPECT_THROW(ForkUnionExecutor::build_from(scheduler),std::logic_error);
}

TEST(ScheduleCompilation, EmptySlotFanInOutPreservesAllDependencies) {
    World world;
    Scheduler scheduler;
    std::atomic<int> before{0},after{0};
    for(auto name:{"A","B"})scheduler.system(name).before("Join").run([&]{++before;}).build();
    for(auto name:{"C","D"})scheduler.system(name).after("Join").run([&]{EXPECT_EQ(before.load(),2);++after;}).build();
    auto plan=schedule::compile_dag(scheduler.meta_world());
    EXPECT_EQ(plan.node_count(),5u); // interior join is retained, with no edge expansion
    TaskflowExecutor::build_from(scheduler)->run(&world);
    EXPECT_EQ(after.load(),2);
}
} // namespace elysia_test::schedule_compilation
