#pragma once
#include <elysia/prefab/prefab.hpp>
#include <array>
#include <fstream>
#include <sstream>

namespace battleship_demo {
namespace p = elysia::prefab;
using elysia::Entity;
using elysia::World;
struct Transform { std::array<double, 3> position; };
struct Hull { double hitpoints; };
struct Identity { std::string callsign; uint32_t faction; };
struct Turret { double traverse_degrees_per_second; };
struct Gun { int barrels; double caliber_mm; double reload_seconds; };
struct Gatling { int rounds_per_minute; };
struct Radar { double range_metres; };
struct Engine { double speed_knots; };
struct SpawnParams {
    std::string callsign;
    std::array<double, 3> position;
    uint32_t faction;
};
inline std::string read_file(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) throw p::Error("Cannot open " + path);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}
inline void remap_bound(p::Bound& bound, const p::NativeEntityMap& map) {
    for (auto& [role, entity] : bound.values) entity = map.resolve(entity);
}
inline void remap_record(p::PrefabEntityId& id, const p::NativeEntityMap& map) {
    id.instance = map.resolve(id.instance);
}
template <class T> inline T& component(World& world, Entity entity) {
    auto* value = world.get_component<T>(entity);
    if (!value) throw p::Error("Battleship is missing component " + std::string(elysia::TypeTraits<T>::name()));
    return *value;
}
inline void patch_ship(World& world, std::span<const Entity> slots, const SpawnParams& params) {
    component<Identity>(world, slots[0]) = Identity{params.callsign, params.faction};
    component<Transform>(world, slots[0]).position = params.position;
}
inline p::NativePrefab prepare(const std::string& json) {
    p::ComponentRegistry components;
    p::NativeCloneRegistry clones;
    // Register decoding and cloning independently. Shared IDs connect the capabilities.
    auto type = [&]<class T>(const char* name) {
        components.register_type<T>(std::string("naval::") + name);
        p::register_native_clone<T>(clones);
    };
    type.template operator()<Transform>("Transform");
    type.template operator()<Hull>("Hull");
    type.template operator()<Identity>("Identity");
    type.template operator()<Turret>("Turret");
    type.template operator()<Gun>("Gun");
    type.template operator()<Gatling>("Gatling");
    type.template operator()<Radar>("Radar");
    type.template operator()<Engine>("Engine");
    p::register_native_clone<p::Local>(clones);
    p::register_native_clone<p::Refs>(clones);
    p::register_native_clone<p::Bound, remap_bound>(clones);
    p::register_native_clone<p::PrefabEntityId, remap_record>(clones);

    p::PrefabRegistry registry(std::move(components));
    registry.load_library(p::read_library(p::parse_json(json)));
    World authoring;
    auto instance = registry.spawn_class(authoring, "naval::battleship");
    if (instance.roots.size() != 1) throw p::Error("Battleship requires one root");
    component<Identity>(authoring, instance.roots[0]);
    component<Transform>(authoring, instance.roots[0]);
    component<Hull>(authoring, instance.roots[0]);
    p::resolve_refs(authoring, true);
    // Decode, substitute parameters, and bind references once. All these local
    // objects may now die; the returned native prefab owns a complete snapshot.
    return p::prepare_native_prefab("naval::battleship", authoring, instance.roots[0], clones,
                                    p::native_parameters<SpawnParams, patch_ship>());
}
inline std::map<uint32_t, Entity> records(World& world, const p::NativeInstance& instance) {
    std::map<uint32_t, Entity> result;
    for (Entity entity : instance.entities) {
        auto* record = world.get_component<p::PrefabEntityId>(entity);
        if (!record || record->instance != instance.root() || !result.emplace(record->local, entity).second)
            throw p::Error("Invalid cloned prefab record identity");
    }
    return result;
}
inline void verify(World& world, const p::NativeInstance& instance) {
    auto ids = records(world, instance);
    if (ids.size() != 11 || elysia::collect_subtree(world, instance.root()) != instance.entities)
        throw p::Error("Incorrect battleship hierarchy");
    if (component<p::Bound>(world, ids.at(0)).values.at("radar") != ids.at(50))
        throw p::Error("Hull radar reference was not remapped");
    for (uint32_t mount : {10u, 20u, 30u}) {
        Entity battery = ids.at(mount + 1);
        component<Gun>(world, battery);
        const auto& refs = component<p::Bound>(world, battery).values;
        if (component<elysia::ChildOf>(world, battery).parent != ids.at(mount) ||
            refs.at("hull") != instance.root() || refs.at("fire_control") != ids.at(50))
            throw p::Error("Battery reference or hierarchy was not remapped");
    }
}
} // namespace battleship_demo
