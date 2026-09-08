#pragma once
#include "registry.hpp"

namespace elysia::prefab {
namespace detail {
inline void *component_data(World &world, Entity e, uint64_t id) {
    auto result = world.index().lookup(e);
    if (result.is_err())
        return nullptr;
    auto *record = result.unwrap();
    auto *arch = static_cast<Archetype<DefaultConfig> *>(record->archetype);
    if (!arch)
        return nullptr;
    auto col = arch->get_column_index(id);
    if (!col)
        return nullptr;
    auto location = arch->table().locate(record->row);
    return location.chunk->component(*col, location.index);
}
} // namespace detail
struct Patch {
    std::string target;
    Object set;
    Object attach;
};

inline bool apply_patch(World &world, const ComponentRegistry &registry, const Patch &patch,
                        const LookupScope &scope = {}) {
    std::string name = patch.target.starts_with("@ref:") ? patch.target.substr(5) : patch.target;
    std::optional<Entity> target;
    world.query<const NameTag, Entity>().each([&](const NameTag &tag, Entity e) {
        if (tag.value != name)
            return;
        if (target)
            throw Error("Ambiguous patch target '" + name + "'");
        target = e;
    });
    if (!target)
        return false;
    struct ComponentUpdate {
        const archive::ComponentFactory *factory;
        Value value;
    };
    std::map<uint64_t, ComponentUpdate> updates;
    for (const auto &[key, patch_value] : patch.set) {
        const auto &fac = registry.resolve(key, scope);
        auto *ptr = detail::component_data(world, *target, fac.type_id);
        if (!ptr)
            throw Error("Patch target lacks component '" + fac.key + "'");
        if (!fac.generic)
            throw Error("Component has no generic codec: " + fac.key);
        auto value = fac.generic->to_generic(ptr);
        detail::merge(value, patch_value);
        updates[fac.type_id] = {&fac, std::move(value)};
    }
    for (const auto &[key, value] : patch.attach) {
        const auto &fac = registry.resolve(key, scope);
        updates[fac.type_id] = {&fac, value};
    }
    World validation;
    auto validation_entity = validation.spawn().entity;
    for (const auto &[id, update] : updates)
        registry.decode(validation, validation_entity, *update.factory, update.value);
    for (const auto &[id, update] : updates)
        registry.decode(world, *target, *update.factory, update.value);
    return true;
}
} // namespace elysia::prefab
