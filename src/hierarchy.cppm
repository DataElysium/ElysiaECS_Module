module;
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <unordered_set>
#include <vector>

export module elysia.hierarchy;
import elysia.world;
import elysia.entity;
import elysia.observer;
import graph;

export namespace elysia {
struct ChildOf { Entity parent; };
struct Children { std::vector<Entity> ids; };

namespace hierarchy_detail {
struct Installed {};
inline void require_alive(World& world, Entity entity) {
    if (!world.index().is_alive(entity))
        throw std::invalid_argument("Hierarchy entity is not alive");
}
}

// Breadth-first traversal follows the maintained child lists, never a world query.
inline std::vector<Entity> collect_subtree(World& world, Entity root) {
    hierarchy_detail::require_alive(world, root);
    std::vector<Entity> entities{root};
    std::unordered_set<uint64_t> seen{root.value};
    for (size_t i = 0; i < entities.size(); ++i) {
        if (const auto* children = world.get_component<Children>(entities[i])) {
            for (Entity child : children->ids) {
                hierarchy_detail::require_alive(world, child);
                if (!seen.insert(child.value).second)
                    throw std::logic_error("Hierarchy contains a cycle or duplicate child");
                entities.push_back(child);
            }
        }
    }
    return entities;
}

// Optional graph snapshot. Live hierarchy access continues to use ChildOf/Children.
inline graph::DirectedGraph<Entity> build_hierarchy_graph(World& world, Entity root) {
    graph::DirectedGraph<Entity> result;
    auto entities = collect_subtree(world, root);
    result.reserve_nodes(entities.size());
    for (Entity entity : entities) result.add_node(entity);
    for (Entity parent : entities) {
        if (const auto* children = world.get_component<Children>(parent))
            for (Entity child : children->ids) result.add_edge(parent, child);
    }
    return result;
}

inline void despawn_subtree(World& world, Entity root) {
    auto entities = collect_subtree(world, root);
    // Children die first: avoids recursive cascades and keeps parent lists valid.
    for (auto it = entities.rbegin(); it != entities.rend(); ++it) world.despawn(*it);
}

// Install before inserting hierarchy components. Registering twice is harmless.
inline void install_hierarchy(World& world) {
    if (world.get_resource<hierarchy_detail::Installed>()) return;
    world.observer().on_add<ChildOf>([&world](Entity child) {
        const auto* relation = world.get_component<ChildOf>(child);
        if (!relation) return;
        const Entity parent = relation->parent;
        hierarchy_detail::require_alive(world, parent);
        if (parent == child) throw std::invalid_argument("Entity cannot parent itself");
        auto* children = world.get_component<Children>(parent);
        if (!children) {
            world.entity(parent).add(Children{});
            children = world.get_component<Children>(parent);
        }
        if (std::find(children->ids.begin(), children->ids.end(), child) == children->ids.end())
            children->ids.push_back(child);
    });
    world.observer().on_remove<ChildOf>([&world](Entity child) {
        const auto* relation = world.get_component<ChildOf>(child);
        if (!relation || !world.index().is_alive(relation->parent)) return;
        if (auto* children = world.get_component<Children>(relation->parent))
            std::erase(children->ids, child);
    });
    world.observer().on_remove<Children>([&world](Entity parent) {
        const auto* children = world.get_component<Children>(parent);
        if (!children) return;
        // Child removal mutates the parent's Children vector through the unlink hook.
        const auto pending = children->ids;
        for (Entity child : pending)
            if (world.index().is_alive(child)) despawn_subtree(world, child);
    });
    world.add_resource(hierarchy_detail::Installed{});
}

inline void detach_child(World& world, Entity child) {
    hierarchy_detail::require_alive(world, child);
    install_hierarchy(world);
    world.entity(child).remove<ChildOf>();
}

inline void reparent(World& world, Entity child, Entity parent) {
    hierarchy_detail::require_alive(world, child);
    hierarchy_detail::require_alive(world, parent);
    install_hierarchy(world);
    const auto* previous = world.get_component<ChildOf>(child);
    if (previous && previous->parent == parent) return;
    // Inspect only this child's descendants. No ancestor lookup or world scan.
    const auto descendants = collect_subtree(world, child);
    if (std::find(descendants.begin(), descendants.end(), parent) != descendants.end())
        throw std::invalid_argument("Reparent would create a hierarchy cycle");
    // Remove while the old parent is still available to the unlink hook.
    world.entity(child).remove<ChildOf>();
    world.entity(child).add(ChildOf{parent});
}

inline void attach_child(World& world, Entity parent, Entity child) {
    reparent(world, child, parent);
}
} // namespace elysia
