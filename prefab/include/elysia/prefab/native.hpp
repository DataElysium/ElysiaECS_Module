#pragma once
#include "elysia/hierarchy.hpp"
#include <concepts>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace elysia::prefab {
// Keys belong to the source world. Values belong to this one destination instance.
struct NativeEntityMap {
    std::unordered_map<Entity, Entity> entities;
    Entity resolve(Entity source) const {
        if (!source) return {};
        auto it = entities.find(source);
        if (it == entities.end())
            throw std::invalid_argument("Native prefab reference leaves the template tree");
        return it->second;
    }
};

struct NativeComponentFunctions {
    void (*clone)(World&, Entity, const void*, const NativeEntityMap&) = nullptr;
};
struct NativeCloneRegistry {
    std::unordered_map<uint64_t, NativeComponentFunctions> components;
};

// Remap, when provided, has signature void(T&, const NativeEntityMap&).
// Apply it to the copy before insertion, so OnAdd observes destination references.
template <class T, auto Remap = nullptr>
    requires std::copy_constructible<T>
inline void register_native_clone(NativeCloneRegistry& registry) {
    static_assert(!std::same_as<T, ChildOf> && !std::same_as<T, Children>,
                  "Native prefab spawning manages hierarchy itself");
    registry.components[TypeTraits<T>::id] = {
        [](World& world, Entity entity, const void* source, const NativeEntityMap& map) {
            T copy = *static_cast<const T*>(source);
            if constexpr (Remap != nullptr) Remap(copy, map);
            world.entity(entity).add(std::move(copy));
        }};
}

struct NativePrefabFunctions {
    uint64_t parameter_type = 0;
    void (*patch)(World&, std::span<const Entity>, const void*) = nullptr;
};

// Patch has signature void(World&, span<const Entity>, const Params&).
// Slots are breadth-first, with the root at slot zero.
template <class Params, auto Patch>
inline NativePrefabFunctions native_parameters() {
    return {TypeTraits<Params>::id,
            [](World& world, std::span<const Entity> slots, const void* parameters) {
                Patch(world, slots, *static_cast<const Params*>(parameters));
            }};
}

struct NativeInstance {
    std::vector<Entity> entities;
    Entity root() const { return entities.at(0); }
};

namespace native_detail {
struct Component {
    const void* data;
    NativeComponentFunctions functions;
};
struct Row {
    Entity source;
    std::optional<size_t> parent;
    std::vector<Component> components;
};
inline std::vector<Row> plan(World& world, Entity root, const NativeCloneRegistry& registry) {
    auto entities = collect_subtree(world, root);
    std::unordered_map<Entity, size_t> slots;
    for (size_t i = 0; i < entities.size(); ++i) slots.emplace(entities[i], i);
    std::unordered_map<Entity, size_t> parents;
    for (size_t i = 0; i < entities.size(); ++i)
        if (const auto* children = world.get_component<Children>(entities[i]))
            for (Entity child : children->ids) parents.emplace(child, i);
    std::vector<Row> rows;
    rows.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        Row row{entities[i], {}, {}};
        if (i != 0) {
            auto* parent = world.get_component<ChildOf>(row.source);
            if (!parent || !slots.contains(parent->parent) || slots.at(parent->parent) >= i)
                throw std::invalid_argument("Native prefab has inconsistent hierarchy");
            row.parent = slots.at(parent->parent);
            if (!parents.contains(row.source) || parents.at(row.source) != *row.parent)
                throw std::invalid_argument("Native prefab has inconsistent hierarchy");
        }
        auto record = world.index().lookup(row.source).unwrap();
        auto* arch = static_cast<Archetype<DefaultConfig>*>(record->archetype);
        auto location = arch->table().locate(record->row);
        size_t column = 0;
        for (const auto* type : arch->types()) {
            if (type->id != TypeTraits<ChildOf>::id && type->id != TypeTraits<Children>::id) {
                auto it = registry.components.find(type->id);
                if (it == registry.components.end() || !it->second.clone)
                    throw std::invalid_argument("Missing native clone for component id " + std::to_string(type->id));
                row.components.push_back({location.chunk->component(column, location.index), it->second});
            }
            ++column;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
inline void discard(World& world, const NativeInstance& instance) {
    for (auto it = instance.entities.rbegin(); it != instance.entities.rend(); ++it)
        if (world.index().is_alive(*it)) world.despawn(*it);
}
inline NativeInstance clone(World& world, const std::vector<Row>& rows,
                            NativePrefabFunctions functions = {}, const void* parameters = nullptr) {
    install_hierarchy(world);
    NativeInstance instance;
    instance.entities.reserve(rows.size());
    try {
        NativeEntityMap map;
        map.entities.reserve(rows.size());
        for (const auto& row : rows) {
            auto entity = world.spawn().entity;
            instance.entities.push_back(entity);
            map.entities.emplace(row.source, entity);
        }
        for (size_t i = 0; i < rows.size(); ++i)
            for (const auto& component : rows[i].components)
                component.functions.clone(world, instance.entities[i], component.data, map);
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].parent)
                world.entity(instance.entities[i]).add(ChildOf{instance.entities[*rows[i].parent]});
        if (functions.patch) functions.patch(world, instance.entities, parameters);
        return instance;
    } catch (...) {
        discard(world, instance);
        throw;
    }
}
struct State {
    std::string name;
    World world;
    std::vector<Row> rows;
    NativePrefabFunctions functions;
};
} // namespace native_detail

class NativePrefab {
  public:
    const std::string& name() const { return state_->name; }
    size_t entity_count() const { return state_->rows.size(); }
    NativePrefabFunctions functions() const { return state_->functions; }

  private:
    explicit NativePrefab(std::shared_ptr<const native_detail::State> state) : state_(std::move(state)) {}
    std::shared_ptr<const native_detail::State> state_;
    friend NativePrefab prepare_native_prefab(std::string, World&, Entity,
                                               const NativeCloneRegistry&, NativePrefabFunctions);
    friend NativeInstance spawn_native(World&, const NativePrefab&);
    template <class Params> friend NativeInstance spawn_native(World&, const NativePrefab&, const Params&);
};

// Snapshot both data and callbacks. Authoring world and registry may then change or die.
// The snapshot's world stays address-stable for hierarchy observers.
inline NativePrefab prepare_native_prefab(std::string name, World& authoring, Entity root,
                                         const NativeCloneRegistry& registry,
                                         NativePrefabFunctions functions = {}) {
    if (name.empty()) throw std::invalid_argument("Empty native prefab name");
    auto source = native_detail::plan(authoring, root, registry);
    auto state = std::make_shared<native_detail::State>();
    state->name = std::move(name);
    state->functions = functions;
    auto instance = native_detail::clone(state->world, source);
    state->rows = native_detail::plan(state->world, instance.root(), registry);
    return NativePrefab(std::move(state));
}
inline NativeInstance spawn_native(World& world, const NativePrefab& prefab) {
    if (prefab.state_->functions.patch)
        throw std::invalid_argument("Native prefab requires typed parameters");
    return native_detail::clone(world, prefab.state_->rows);
}
template <class Params>
inline NativeInstance spawn_native(World& world, const NativePrefab& prefab, const Params& parameters) {
    const auto functions = prefab.state_->functions;
    if (!functions.patch || functions.parameter_type != TypeTraits<Params>::id)
        throw std::invalid_argument("Native prefab parameter type mismatch");
    return native_detail::clone(world, prefab.state_->rows, functions, &parameters);
}

// A failed batch removes all entities created by this batch.
template <class Params>
inline std::vector<NativeInstance> spawn_native_batch(World& world, const NativePrefab& prefab,
                                                       std::span<const Params> parameters) {
    std::vector<NativeInstance> result;
    result.reserve(parameters.size());
    try {
        for (const auto& value : parameters) result.push_back(spawn_native(world, prefab, value));
    } catch (...) {
        for (auto it = result.rbegin(); it != result.rend(); ++it) native_detail::discard(world, *it);
        throw;
    }
    return result;
}
inline std::vector<NativeInstance> spawn_native_batch(World& world, const NativePrefab& prefab, size_t count) {
    std::vector<NativeInstance> result;
    result.reserve(count);
    try {
        for (size_t i = 0; i < count; ++i) result.push_back(spawn_native(world, prefab));
    } catch (...) {
        for (auto it = result.rbegin(); it != result.rend(); ++it) native_detail::discard(world, *it);
        throw;
    }
    return result;
}
} // namespace elysia::prefab
