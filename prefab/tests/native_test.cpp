#include <gtest/gtest.h>
#include <elysia/prefab/native.hpp>
#include <array>

namespace {
namespace p = elysia::prefab;
using namespace elysia;
struct Hull { std::vector<int> armor; };
struct Turret { Entity hull; int damage; };
struct Params { int armor; int damage; };
struct WrongParams { int armor; int damage; };
void remap_turret(Turret& turret, const p::NativeEntityMap& map) {
    turret.hull = map.resolve(turret.hull);
}
void patch_ship(World& world, std::span<const Entity> entities, const Params& params) {
    world.get_component<Hull>(entities[0])->armor[0] = params.armor;
    world.get_component<Turret>(entities[1])->damage = params.damage;
}
p::NativeCloneRegistry types() {
    p::NativeCloneRegistry registry;
    p::register_native_clone<Hull>(registry);
    p::register_native_clone<Turret, remap_turret>(registry);
    return registry;
}
Entity ship(World& world) {
    auto hull = world.spawn().add(Hull{{10, 20}}).entity;
    auto turret = world.spawn().add(Turret{hull, 30}).entity;
    attach_child(world, hull, turret);
    return hull;
}
size_t count(World& world) {
    size_t result = 0;
    world.query<Entity>().each([&](Entity) { ++result; });
    return result;
}
p::NativePrefab prepared(p::NativePrefabFunctions functions = {}) {
    World authoring;
    auto root = ship(authoring);
    return p::prepare_native_prefab("ship", authoring, root, types(), functions);
}

TEST(NativePrefab, OwnsSnapshotAndClonesIndependentTrees) {
    auto prefab = prepared(); // authoring world and registry have been destroyed
    EXPECT_EQ(prefab.name(), "ship");
    EXPECT_EQ(prefab.entity_count(), 2u);
    EXPECT_EQ(prefab.functions().patch, nullptr);
    World target;
    // Ensure template and target IDs differ.
    for (int i = 0; i < 5; ++i) target.spawn();
    int observed = 0;
    target.observer().on_add<Turret>([&](Entity entity) {
        auto* turret = target.get_component<Turret>(entity);
        EXPECT_TRUE(target.index().is_alive(turret->hull));
        EXPECT_GE(turret->hull.id(), 5u);
        ++observed;
    });
    auto instances = p::spawn_native_batch(target, prefab, size_t{2});
    ASSERT_EQ(instances.size(), 2u);
    EXPECT_EQ(observed, 2);
    for (const auto& instance : instances) {
        EXPECT_EQ(collect_subtree(target, instance.root()), instance.entities);
        EXPECT_EQ(target.get_component<Turret>(instance.entities[1])->hull, instance.root());
    }
    target.get_component<Hull>(instances[0].root())->armor[0] = 999;
    EXPECT_EQ(target.get_component<Hull>(instances[1].root())->armor[0], 10);
    World other;
    auto third = p::spawn_native(other, prefab);
    EXPECT_EQ(other.get_component<Hull>(third.root())->armor[0], 10);
}

TEST(NativePrefab, TypedBatchPatchesAndRejectsWrongTypesBeforeMutation) {
    auto prefab = prepared(p::native_parameters<Params, patch_ship>());
    World target;
    auto sentinel = target.spawn().entity;
    EXPECT_THROW(p::spawn_native(target, prefab), std::invalid_argument);
    EXPECT_THROW(p::spawn_native(target, prefab, WrongParams{1, 2}), std::invalid_argument);
    EXPECT_EQ(count(target), 1u);
    std::array<Params, 2> params{{{40, 50}, {60, 70}}};
    auto batch = p::spawn_native_batch(target, prefab, std::span<const Params>(params));
    for (size_t i = 0; i < batch.size(); ++i) {
        EXPECT_EQ(target.get_component<Hull>(batch[i].root())->armor[0], params[i].armor);
        EXPECT_EQ(target.get_component<Turret>(batch[i].entities[1])->damage, params[i].damage);
    }
    EXPECT_TRUE(target.index().is_alive(sentinel));
}

TEST(NativePrefab, SourceChangesDoNotChangePreparedDataOrCallbacks) {
    World authoring;
    auto root = ship(authoring);
    auto registry = types();
    auto prefab = p::prepare_native_prefab("ship", authoring, root, registry);
    registry.components.clear();
    authoring.get_component<Hull>(root)->armor[0] = 100;
    auto extra = authoring.spawn().add(Hull{{99}}).entity;
    attach_child(authoring, root, extra);
    World target;
    auto instance = p::spawn_native(target, prefab);
    EXPECT_EQ(instance.entities.size(), 2u);
    EXPECT_EQ(target.get_component<Hull>(instance.root())->armor[0], 10);
}

TEST(NativePrefab, MissingCloneAndExternalReferencesAreRejected) {
    World authoring;
    auto root = ship(authoring);
    p::NativeCloneRegistry missing;
    EXPECT_THROW(p::prepare_native_prefab("ship", authoring, root, missing), std::invalid_argument);
    auto registry = types();
    registry.components[TypeTraits<Hull>::id] = {};
    EXPECT_THROW(p::prepare_native_prefab("ship", authoring, root, registry), std::invalid_argument);
    auto external = authoring.spawn().entity;
    auto child = authoring.get_component<Children>(root)->ids[0];
    authoring.get_component<Turret>(child)->hull = external;
    EXPECT_THROW(p::prepare_native_prefab("ship", authoring, root, types()), std::invalid_argument);
    EXPECT_EQ(count(authoring), 3u);
}

void fail_patch(World&, std::span<const Entity>, const Params& params) {
    if (params.damage < 0) throw std::runtime_error("patch failed");
}
TEST(NativePrefab, FailedPatchRollsBackEntireBatch) {
    auto prefab = prepared(p::native_parameters<Params, fail_patch>());
    World target;
    auto sentinel = target.spawn().add(Hull{{42}}).entity;
    std::array<Params, 2> params{{{1, 2}, {1, -1}}};
    EXPECT_THROW(p::spawn_native_batch(target, prefab, std::span<const Params>(params)), std::runtime_error);
    EXPECT_EQ(count(target), 1u);
    EXPECT_EQ(target.get_component<Hull>(sentinel)->armor[0], 42);
    auto success = p::spawn_native(target, prefab, Params{3, 4});
    EXPECT_EQ(collect_subtree(target, success.root()), success.entities);
}

struct Custom { std::unique_ptr<int> value; };
TEST(NativePrefab, CustomCloneSupportsNonCopyableComponentsAndRollback) {
    p::NativeCloneRegistry registry;
    registry.components[TypeTraits<Custom>::id].clone =
        [](World& world, Entity entity, const void* source, const p::NativeEntityMap&) {
            const auto& value = *static_cast<const Custom*>(source);
            world.entity(entity).add(Custom{std::make_unique<int>(*value.value)});
        };
    World source;
    auto root = source.spawn().add(Custom{std::make_unique<int>(17)}).entity;
    auto prefab = p::prepare_native_prefab("custom", source, root, registry);
    World target;
    auto first = p::spawn_native(target, prefab);
    EXPECT_EQ(*target.get_component<Custom>(first.root())->value, 17);
    target.observer().on_add<Custom>([](Entity) { throw std::runtime_error("observer failed"); });
    EXPECT_THROW(p::spawn_native(target, prefab), std::runtime_error);
    EXPECT_EQ(count(target), 1u);
    EXPECT_EQ(*source.get_component<Custom>(root)->value, 17);
}

TEST(NativePrefab, EmptyEntityAndNullReferenceCanBeCloned) {
    p::NativeEntityMap map;
    EXPECT_EQ(map.resolve(Entity{}), Entity{});
    World source;
    auto root = source.spawn().entity;
    auto prefab = p::prepare_native_prefab("empty", source, root, {});
    World target;
    auto instance = p::spawn_native(target, prefab);
    EXPECT_EQ(instance.entities.size(), 1u);
    EXPECT_TRUE(target.index().is_alive(instance.root()));
}
TEST(NativePrefab, ForwardReferencesAndSubtreeRootAreRemapped) {
    World authoring;
    auto external_parent = authoring.spawn().entity;
    auto root = authoring.spawn().add(Hull{{10}}).entity;
    auto first = authoring.spawn().entity;
    auto second = authoring.spawn().entity;
    authoring.entity(first).add(Turret{second, 1});
    authoring.entity(second).add(Turret{first, 2});
    attach_child(authoring, external_parent, root);
    attach_child(authoring, root, first);
    attach_child(authoring, root, second);
    auto prefab = p::prepare_native_prefab("subtree", authoring, root, types());
    World target;
    auto instance = p::spawn_native(target, prefab);
    ASSERT_EQ(instance.entities.size(), 3u);
    EXPECT_EQ(target.get_component<ChildOf>(instance.root()), nullptr);
    EXPECT_EQ(target.get_component<Turret>(instance.entities[1])->hull, instance.entities[2]);
    EXPECT_EQ(target.get_component<Turret>(instance.entities[2])->hull, instance.entities[1]);
    EXPECT_EQ(collect_subtree(target, instance.root()), instance.entities);
}

TEST(NativePrefab, RejectsInconsistentHierarchy) {
    World authoring;
    auto root = ship(authoring);
    auto child = authoring.get_component<Children>(root)->ids[0];
    // Simulate an unsupported raw relationship write.
    authoring.get_component<ChildOf>(child)->parent = child;
    EXPECT_THROW(p::prepare_native_prefab("broken", authoring, root, types()), std::invalid_argument);
    authoring.get_component<ChildOf>(child)->parent = root;
}
}
