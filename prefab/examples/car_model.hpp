#pragma once
#include <elysia/prefab/prefab.hpp>
#include <array>
#include <fstream>
#include <sstream>

namespace car_demo {
struct Transform { std::array<double, 3> position; std::array<double, 3> size; double rotation; };
struct Visual { std::array<double, 4> color; };
struct Velocity { std::array<double, 2> vec; };
struct Gravity { double force; };
struct PlayerControl { double speed; double jump_force; };
struct Collider { std::array<double, 2> half_extents; };
struct CarBody {};
struct CarWheel { std::array<double, 2> offset; double spin_factor; };
struct Spin { double speed; };

inline elysia::prefab::ComponentRegistry registry() {
    elysia::prefab::ComponentRegistry registry;
    registry.register_type<Transform>("Transform");
    registry.register_type<Visual>("Visual");
    registry.register_type<Velocity>("Velocity");
    registry.register_type<Gravity>("Gravity");
    registry.register_type<PlayerControl>("PlayerControl");
    registry.register_type<Collider>("Collider");
    registry.register_type<CarBody>("CarBody");
    registry.register_type<CarWheel>("CarWheel");
    registry.register_type<Spin>("Spin");
    return registry;
}
inline elysia::prefab::Value read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw elysia::prefab::Error("Cannot open " + path);
    std::ostringstream text; text << in.rdbuf();
    return elysia::prefab::parse_json(text.str());
}
inline elysia::prefab::Spawned load_game(elysia::World& world, elysia::prefab::PrefabRegistry& registry,
                                      const elysia::prefab::Value& manifest) {
    namespace p = elysia::prefab;
    auto document = p::read_document(manifest);
    registry.load_library(std::move(document.prefabs));
    p::Spawned all;
    for (const auto& instance : document.instances) {
        auto spawned = registry.spawn_class(world, instance.prefab, instance.params, instance.id + ".");
        if (spawned.roots.size() != 1) throw p::Error("Game instance requires one root");
        world.entity(spawned.roots.front()).add(p::NameTag{instance.id});
        all.roots.insert(all.roots.end(), spawned.roots.begin(), spawned.roots.end());
        all.entities.insert(all.entities.end(), spawned.entities.begin(), spawned.entities.end());
    }
    return all;
}
inline elysia::prefab::Value normalized(elysia::World& world, const elysia::prefab::PrefabRegistry& registry,
                                      const elysia::prefab::Spawned& spawned) {
    namespace p = elysia::prefab;
    std::function<std::string(elysia::Entity)> path = [&](elysia::Entity e) -> std::string {
        if (auto* parent = world.get_component<p::ChildOf>(e))
            return path(parent->parent) + "/" + std::to_string(world.get_component<p::PrefabEntityId>(e)->local);
        return world.get_component<p::NameTag>(e)->value;
    };
    std::map<std::string, p::Value> rows;
    for (auto e : spawned.entities) {
        p::Object components;
        for (const auto& [id, fac] : registry.components().archive_registry().factories())
            if (auto* ptr = p::detail::component_data(world, e, id); ptr && fac.generic)
                components[fac.key] = fac.generic->to_generic(ptr);
        rows[path(e)] = std::move(components);
    }
    p::Object result;
    for (auto& [path, components] : rows) result[path] = std::move(components);
    return result;
}
}
