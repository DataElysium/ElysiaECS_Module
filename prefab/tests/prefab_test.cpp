#include <gtest/gtest.h>
#include "../examples/car_model.hpp"

namespace {
namespace p = elysia::prefab;
struct Resistance { double ohms; };
struct ScalarResistanceWire {
    using ReflectionType = double;
    double value;
    explicit ScalarResistanceWire(double input) : value(input) {}
    double reflection() const { return value; }
    static ScalarResistanceWire from(const Resistance& r) { return ScalarResistanceWire(r.ohms); }
    Resistance into() const { return {value}; }
};
struct OtherResistance { double ohms; };
struct RuntimeHistory { double previous; };
struct Pair { int a; int b; };
p::Library library(const std::string& text) { return p::read_library(p::parse_json(text)); }
size_t entity_count(elysia::World& world) {
    size_t count = 0; world.query<elysia::Entity>().each([&](auto) { ++count; }); return count;
}

TEST(Prefab, UnchangedRustCarManifest) {
    elysia::World world;
    p::PrefabRegistry registry(car_demo::registry());
    auto result = car_demo::load_game(world, registry, car_demo::read_file("fixtures/game.json"));
    EXPECT_EQ(result.roots.size(), 3);
    EXPECT_EQ(result.entities.size(), 11);
    size_t wheels = 0;
    world.query<const car_demo::CarWheel, const p::ChildOf>().each([&](const auto& wheel, const auto& parent) {
        EXPECT_EQ(std::abs(wheel.offset[0]), 52);
        EXPECT_TRUE(world.get_component<p::ChildOf>(parent.parent));
        ++wheels;
    });
    EXPECT_EQ(wheels, 4);
    EXPECT_NE(world.get_component<car_demo::PlayerControl>(result.roots[0]), nullptr);
}
TEST(Prefab, HierarchyIsAvailableImmediatelyWithoutAWorldScan) {
    static_assert(std::is_same_v<p::ChildOf, elysia::ChildOf>);
    static_assert(std::is_same_v<p::Children, elysia::Children>);
    elysia::World world;
    p::PrefabRegistry registry(car_demo::registry());
    auto spawned = car_demo::load_game(world, registry, car_demo::read_file("fixtures/game.json"));
    std::set<uint64_t> reached;
    for (auto root : spawned.roots) {
        auto direct = elysia::collect_subtree(world, root);
        auto graph = elysia::build_hierarchy_graph(world, root);
        EXPECT_EQ(graph.node_count(), direct.size());
        for (auto entity : direct) {
            EXPECT_TRUE(graph.has_node(entity));
            EXPECT_TRUE(reached.insert(entity.value).second);
        }
    }
    EXPECT_EQ(reached.size(), 11);
    for (auto entity : spawned.entities) EXPECT_TRUE(reached.contains(entity.value));
    auto car = elysia::build_hierarchy_graph(world, spawned.roots[0]);
    EXPECT_EQ(car.node_count(), 9);
    EXPECT_EQ(car.out_degree(car.id(spawned.roots[0])), 4);
}
TEST(Prefab, TemplateGraphPreservesOutOfOrderParentsAndFailedReload) {
    p::PrefabRegistry registry;
    registry.load_library(library(R"({"tree":{"body":[
        {"id":9,"parent":2,"components":{}},
        {"id":2,"parent":7,"components":{}},
        {"id":7,"components":{}}]}})"));
    auto root = registry.template_root("tree");
    auto graph = registry.template_graph("tree");
    EXPECT_EQ(graph.node_count(), 4);
    EXPECT_EQ(graph.out_degree(graph.id(root)), 1);
    EXPECT_THROW(registry.load_library(library(R"({"tree":{"body":[
        {"id":1,"parent":2,"components":{}},{"id":2,"parent":1,"components":{}}]}})")), p::Error);
    EXPECT_EQ(registry.template_root("tree"), root);
    EXPECT_EQ(registry.template_graph("tree").node_count(), 4);
    elysia::World world;
    auto spawned = registry.spawn_class(world, "tree");
    auto top_down = elysia::collect_subtree(world, spawned.roots[0]);
    ASSERT_EQ(top_down.size(), 3);
    EXPECT_EQ(world.get_component<p::PrefabEntityId>(top_down[0])->local, 7);
    EXPECT_EQ(world.get_component<p::PrefabEntityId>(top_down[1])->local, 2);
    EXPECT_EQ(world.get_component<p::PrefabEntityId>(top_down[2])->local, 9);
}
TEST(Prefab, SpawnFailureCleansUpMaintainedHierarchy) {
    p::ComponentRegistry types;
    types.register_type<Pair>("Pair");
    p::PrefabRegistry registry(types);
    registry.load_library(library(R"({"tree":{"body":[
        {"id":0,"components":{}},
        {"id":1,"parent":0,"components":{}},
        {"id":2,"parent":1,"components":{"Pair":{"a":1,"b":2}}}]}})"));
    elysia::World world;
    auto unrelated = world.spawn().entity;
    world.observer().on_add<Pair>([](auto) { throw std::runtime_error("insertion failed"); });
    EXPECT_THROW(registry.spawn_class(world, "tree"), std::runtime_error);
    EXPECT_EQ(entity_count(world), 1);
    EXPECT_TRUE(world.index().is_alive(unrelated));
}
TEST(Prefab, DefaultRegistrationUsesNativeFullName) {
    p::ComponentRegistry components;
    components.register_type<car_demo::Transform>();
    const auto& factory = components.resolve("car_demo::Transform");
    EXPECT_EQ(factory.key, elysia::TypeTraits<car_demo::Transform>::name());
    EXPECT_EQ(factory.type_id, elysia::TypeTraits<car_demo::Transform>::id);
    EXPECT_EQ(components.resolve("Transform", {.current="car_demo"}).type_id, factory.type_id);
    p::PrefabRegistry registry(std::move(components));
    registry.load_library(library(R"({"car_demo::object":{"body":[{"id":0,"components":{
        "Transform":{"position":[1,2,3],"size":[4,5,6],"rotation":7}
    }}]}})"));
    elysia::World world;
    auto instance = registry.spawn_class(world, "car_demo::object");
    auto* transform = world.get_component<car_demo::Transform>(instance.roots.at(0));
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->rotation, 7);
}
TEST(Prefab, NativeIdsAndScopedNamesAreIndependent) {
    p::ComponentRegistry registry;
    registry.register_type<Resistance>("electrical::Inductor");
    registry.register_type<OtherResistance>("other::Inductor");
    EXPECT_EQ(registry.resolve("Inductor", {.current="electrical"}).type_id, elysia::TypeTraits<Resistance>::id);
    EXPECT_EQ(registry.resolve("other::Inductor").type_id, elysia::TypeTraits<OtherResistance>::id);
    EXPECT_THROW(registry.resolve("Inductor", {.imports={"electrical", "other"}}), p::Error);
    EXPECT_EQ(registry.resolve("L", {.aliases={{"L", "electrical::Inductor"}}}).key, "electrical::Inductor");
    EXPECT_THROW(registry.register_type<Pair>("electrical::Inductor"), p::Error);
    p::ComponentRegistry names;
    names.register_type<Resistance>("R");
    names.register_type<OtherResistance>("circuit::R");
    EXPECT_EQ(names.resolve("R", {.imports={"circuit"}}).type_id, elysia::TypeTraits<OtherResistance>::id);
    EXPECT_EQ(names.resolve("R", {.aliases={{"R", "circuit::R"}}}).type_id, elysia::TypeTraits<OtherResistance>::id);
    EXPECT_EQ(names.resolve("::R", {.imports={"circuit"}}).type_id, elysia::TypeTraits<Resistance>::id);
}
TEST(Prefab, ComponentRegistryUsesExternallySelectedArchive) {
    elysia::archive::SnapshotRegistry cases;
    cases.register_type<Resistance>("case::R");
    ASSERT_NE(cases.find("case::R"), nullptr);
    EXPECT_EQ(cases.find("case::R")->type_id, elysia::TypeTraits<Resistance>::id);
    EXPECT_EQ(cases.find(""), nullptr);
    p::ComponentRegistry components(cases);
    EXPECT_EQ(components.resolve("case::R").type_id, elysia::TypeTraits<Resistance>::id);
    EXPECT_THROW(components.resolve(std::string(elysia::TypeTraits<RuntimeHistory>::name())), p::Error);
}
TEST(Prefab, ArchiveRegistrationIsSharedAcrossLibrariesAndWorlds) {
    elysia::archive::SnapshotRegistry archive;
    p::PrefabRegistry first(archive);
    p::PrefabRegistry second(archive);
    EXPECT_EQ(&first.archive_registry(), &archive);
    EXPECT_EQ(&second.archive_registry(), &archive);

    // Direct archive registration after constructing both prefab libraries is visible.
    archive.register_type<Pair>("case::Pair");
    auto definitions = library(R"({"case::item":{"body":[{"id":0,"components":{"Pair":{"a":3,"b":7}}}]}})");
    first.load_library(definitions);
    second.load_library(definitions);
    elysia::World first_world;
    elysia::World second_world;
    auto a = first.spawn_class(first_world, "case::item").roots.at(0);
    auto b = second.spawn_class(second_world, "case::item").roots.at(0);
    ASSERT_NE(first_world.get_component<Pair>(a), nullptr);
    ASSERT_NE(second_world.get_component<Pair>(b), nullptr);
    EXPECT_EQ(first_world.get_component<Pair>(a)->b, 7);
    EXPECT_EQ(second_world.get_component<Pair>(b)->b, 7);

    // Registration through prefab is available to ordinary archive callers too.
    first.components().register_type<Resistance>("case::R");
    ASSERT_NE(archive.find("case::R"), nullptr);
    EXPECT_EQ(second.components().resolve("case::R").type_id, elysia::TypeTraits<Resistance>::id);
    auto json = archive.find("case::Pair")->generic->to_generic(first_world.get_component<Pair>(a));
    EXPECT_EQ(p::detail::required(p::detail::object(json), "b").to_int64().value(), 7);
}
TEST(Prefab, CopiedOwnedRegistryKeepsSharedStorageAlive) {
    auto make_library = [] {
        p::ComponentRegistry components;
        p::PrefabRegistry prefabs(components);
        components.register_type<Pair>("Pair");
        return prefabs;
    };
    auto prefabs = make_library();
    EXPECT_EQ(prefabs.components().resolve("Pair").type_id, elysia::TypeTraits<Pair>::id);
    prefabs.load_library(library(R"({"item":{"body":[{"id":0,"components":{"Pair":{"a":1,"b":2}}}]}})"));
    elysia::World world;
    auto entity = prefabs.spawn_class(world, "item").roots.at(0);
    ASSERT_NE(world.get_component<Pair>(entity), nullptr);
    EXPECT_EQ(world.get_component<Pair>(entity)->a, 1);
}
TEST(Prefab, ExternalRegistryChangesDoNotLeaveStaleNames) {
    elysia::archive::SnapshotRegistry archive;
    archive.register_type<Resistance>("old::R");
    p::ComponentRegistry components(archive);
    archive.register_type<Resistance>("new::R");
    EXPECT_THROW(components.resolve("old::R"), p::Error);
    EXPECT_EQ(components.resolve("new::R").type_id, elysia::TypeTraits<Resistance>::id);
    archive.register_type<OtherResistance>("new::R");
    EXPECT_THROW(components.resolve("new::R"), p::Error);
}
TEST(Prefab, CircuitStructuredParametersAndGlobals) {
    p::ComponentRegistry components;
    components.register_type<Resistance>("circuit::Resistor");
    p::PrefabRegistry registry(std::move(components));
    registry.register_global("gnd", p::Value(std::string("gnd")));
    auto lib = library(R"({
      "circuit::branch":{"params":{"physical":{"r":null},"ports":{"p":null,"n":"@gnd"}},
        "body":[{"id":0,"components":{"Resistor":{"ohms":"$physical.r"},"Net":["$ports.p","$ports.n"]}}]},
      "circuit::load":{"params":{"r":10.0,"bus":"a"},"body":[{"id":0,"components":{
        "Use":{"prefab":"branch","params":{"physical":{"r":"$r"},"ports":{"p":"$bus"}}}}}]}
    })");
    registry.load_library(lib);
    elysia::World world;
    registry.spawn_class(world, "circuit::load", p::detail::object(p::parse_json(R"({"r":25.0,"bus":"bus_a"})")));
    world.query<const Resistance>().each([](const Resistance& r) { EXPECT_EQ(r.ohms, 25); });
    auto nets = p::lower_nets(world);
    EXPECT_EQ(nets.at("gnd"), 0);
    EXPECT_TRUE(nets.contains("bus_a"));
    EXPECT_EQ(p::detail::string(p::detail::required(registry.export_class("circuit::branch").body[0].components, "Resistor").to_object().value().at("ohms")), "$physical.r");
}
TEST(Prefab, UnchangedRustCircuitUsesScalarProxy) {
    p::ComponentRegistry components;
    components.register_proxy<Resistance, ScalarResistanceWire>("Resistor");
    ASSERT_NE(components.archive_registry().find("Resistor"), nullptr);
    EXPECT_EQ(components.archive_registry().find("Resistor")->type_id, elysia::TypeTraits<Resistance>::id);
    p::PrefabRegistry registry(std::move(components));
    registry.register_global("gnd", p::Value(std::string("gnd")));
    registry.load_library(p::read_library(car_demo::read_file("fixtures/circuit.json")));
    elysia::World world;
    registry.spawn_class(world, "pair");
    std::multiset<double> resistances;
    std::multiset<std::string> nets;
    world.query<const Resistance, const p::Net>().each([&](const Resistance& r, const p::Net& net) {
        resistances.insert(r.ohms);
        nets.insert(net.values.begin(), net.values.end());
    });
    EXPECT_EQ(resistances, (std::multiset<double>{2, 2, 3, 3}));
    EXPECT_EQ(nets, (std::multiset<std::string>{"x", "0.mid", "0.mid", "gnd", "y", "1.mid", "1.mid", "gnd"}));
}
TEST(Prefab, RepeatedInstancesAndReferences) {
    p::PrefabRegistry registry;
    registry.load_library(library(R"({"pair":{"body":[
      {"id":0,"components":{}},
      {"id":1,"parent":0,"components":{"Local":"a","Refs":{"self":"$id:1","peer":"$ref:b","outside":"@ref:external"}}},
      {"id":2,"parent":0,"components":{"Local":"b"}}]}})"));
    elysia::World world;
    auto external = world.spawn().add(p::NameTag{"external"}).entity;
    auto a = registry.spawn_class(world, "pair");
    auto b = registry.spawn_class(world, "pair");
    p::resolve_refs(world, true);
    auto* bound = world.get_component<p::Bound>(a.entities[1]);
    ASSERT_NE(bound, nullptr);
    EXPECT_EQ(bound->values.at("self"), a.entities[1]);
    EXPECT_EQ(bound->values.at("peer"), a.entities[2]);
    EXPECT_EQ(bound->values.at("outside"), external);
    EXPECT_NE(bound->values.at("peer"), b.entities[2]);
    world.despawn(external);
    p::resolve_refs(world);
    EXPECT_FALSE(world.get_component<p::Bound>(a.entities[1])->values.contains("outside"));
}
TEST(Prefab, ErrorsLeaveDestinationUntouched) {
    p::ComponentRegistry components; components.register_type<Pair>("Pair");
    p::PrefabRegistry registry(std::move(components));
    registry.load_library(library(R"({"bad":{"params":{"a":0},"body":[{"id":0,"components":{"Pair":{"a":"$a","b":2}}}]}})"));
    elysia::World world; world.spawn().add(Pair{9, 8});
    EXPECT_THROW(registry.spawn_class(world, "bad", p::detail::object(p::parse_json(R"({"a":"wrong"})"))), p::Error);
    EXPECT_EQ(entity_count(world), 1);
    EXPECT_THROW(registry.spawn_class(world, "bad", p::detail::object(p::parse_json(R"({"typo":1})"))), p::Error);
}
TEST(Prefab, InvalidHierarchyAndComposition) {
    p::PrefabRegistry registry;
    EXPECT_THROW(registry.load_library(library(R"({"x":{"body":[{"id":0,"parent":0,"components":{}}]}})")), p::Error);
    EXPECT_THROW(registry.load_library(library(R"({"x":{"body":[{"id":0,"parent":5,"components":{}}]}})")), p::Error);
    EXPECT_THROW(registry.load_library(library(R"({"x":{"body":[{"id":0,"components":{"Missing":{}}}]}})")), p::Error);
    registry.load_library(library(R"({"x":{"body":[{"id":0,"components":{"Use":{"prefab":"x"}}}]}})"));
    elysia::World world;
    EXPECT_THROW(registry.spawn_class(world, "x"), p::Error);
    EXPECT_EQ(entity_count(world), 0);
}
TEST(Prefab, JsonPatchPreservesUnmentionedFields) {
    p::ComponentRegistry registry; registry.register_type<Pair>("Pair");
    elysia::World world;
    auto e = world.spawn().add(Pair{1,2}).add(p::NameTag{"target"}).entity;
    EXPECT_TRUE(p::apply_patch(world, registry, {"@ref:target", p::detail::object(p::parse_json(R"({"Pair":{"a":9}})")), {}}));
    EXPECT_EQ(world.get_component<Pair>(e)->a, 9);
    EXPECT_EQ(world.get_component<Pair>(e)->b, 2);
    EXPECT_FALSE(p::apply_patch(world, registry, {"absent", {}, {}}));
}
TEST(Prefab, NestedNetPortsStayConnectedAndInstancesStaySeparate) {
    p::PrefabRegistry registry;
    registry.register_global("gnd", p::Value(std::string("gnd")));
    registry.load_library(library(R"({
      "leaf":{"params":{"p":null,"n":null},"body":[{"id":0,"components":{"Net":["$p","$n"]}}]},
      "branch":{"params":{"p":null},"body":[
        {"id":0,"components":{"Net":["$p","mid"]}},
        {"id":1,"components":{"Use":{"prefab":"leaf","params":{"p":"mid","n":"@gnd"}}}}]},
      "pair":{"body":[
        {"id":0,"components":{"Use":{"prefab":"branch","params":{"p":"a"}}}},
        {"id":1,"components":{"Use":{"prefab":"branch","params":{"p":"b"}}}}]}
    })"));
    elysia::World world;
    registry.spawn_class(world, "pair");
    std::multiset<std::string> nets;
    world.query<const p::Net>().each([&](const p::Net& net) { nets.insert(net.values.begin(), net.values.end()); });
    EXPECT_EQ(nets.count("0.mid"), 2);
    EXPECT_EQ(nets.count("1.mid"), 2);
    EXPECT_EQ(nets.count("gnd"), 2);
    EXPECT_EQ(nets.count("a"), 1);
    EXPECT_EQ(nets.count("b"), 1);
}
TEST(Prefab, RequiredParamsAndGlobalCyclesReportErrors) {
    p::PrefabRegistry registry;
    registry.load_library(library(R"({"x":{"params":{"ports":{"a":null}},"body":[]}})"));
    elysia::World world;
    try { registry.spawn_class(world, "x"); FAIL(); }
    catch (const p::Error& error) { EXPECT_NE(std::string(error.what()).find("ports.a"), std::string::npos); }
    registry.register_global("a", p::Value(std::string("@b")));
    registry.register_global("b", p::Value(std::string("@a")));
    registry.load_library(library(R"({"x":{"params":{"value":"@a"},"body":[]}})"));
    EXPECT_THROW(registry.spawn_class(world, "x"), p::Error);
}
TEST(Prefab, AuthoringExportPreservesExpressions) {
    p::PrefabRegistry registry;
    auto original = library(R"({"x":{"params":{"value":"name"},"body":[{"id":8,"components":{"Local":"$value"}}]}})" );
    registry.load_library(original);
    EXPECT_EQ(elysia::reflect::write_json(p::write_library(registry.export_library())),
              elysia::reflect::write_json(p::write_library(original)));
}
}
int main(int argc, char** argv) { testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
