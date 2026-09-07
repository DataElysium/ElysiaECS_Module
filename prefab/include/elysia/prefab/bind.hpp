#pragma once
#include "registry.hpp"
#include <charconv>

namespace elysia::prefab {
// Persistent reference strings stay in Refs; Bound is a replaceable runtime cache.
inline void resolve_refs(World& world, bool reject_missing = false) {
    std::map<std::string, Entity> globals;
    std::map<std::pair<uint64_t, std::string>, Entity> siblings;
    std::map<std::pair<uint64_t, uint32_t>, Entity> ids;
    world.query<const NameTag, Entity>().each([&](const NameTag& name, Entity e) {
        if (!globals.emplace(name.value, e).second) throw Error("Duplicate global entity name '" + name.value + "'");
    });
    world.query<const Local, const ChildOf, Entity>().each([&](const Local& name, const ChildOf& parent, Entity e) {
        if (!siblings.emplace(std::pair{parent.parent.value, name.value}, e).second)
            throw Error("Duplicate sibling name '" + name.value + "'");
    });
    world.query<const PrefabEntityId, Entity>().each([&](const PrefabEntityId& id, Entity e) {
        if (!ids.emplace(std::pair{id.instance.value, id.local}, e).second) throw Error("Duplicate prefab instance record id");
    });
    std::vector<std::pair<Entity, Bound>> jobs;
    world.query<const Refs, Entity>().each([&](const Refs& refs, Entity e) {
        Bound bound;
        for (const auto& [role, ref] : refs.values) {
            std::optional<Entity> target;
            if (ref.starts_with("@ref:")) {
                if (auto it = globals.find(ref.substr(5)); it != globals.end()) target = it->second;
            } else if (ref.starts_with("$ref:")) {
                if (auto* p = world.get_component<ChildOf>(e))
                    if (auto it = siblings.find({p->parent.value, ref.substr(5)}); it != siblings.end()) target = it->second;
            } else if (ref.starts_with("$id:")) {
                uint32_t id;
                auto [end, ec] = std::from_chars(ref.data() + 4, ref.data() + ref.size(), id);
                if (ec == std::errc{} && end == ref.data() + ref.size())
                    if (auto* p = world.get_component<PrefabEntityId>(e))
                        if (auto it = ids.find({p->instance.value, id}); it != ids.end()) target = it->second;
            }
            if (target) bound.values[role] = *target;
            else if (reject_missing) throw Error("Unresolved entity reference '" + ref + "' for role '" + role + "'");
        }
        jobs.emplace_back(e, std::move(bound));
    });
    for (auto& [e, bound] : jobs) world.entity(e).add(std::move(bound));
}

// Optional electrical lowering: keep symbolic Net data, derive numeric Pins.
inline std::map<std::string, uint32_t> lower_nets(World& world,
        std::map<std::string, uint32_t> bindings = {{"gnd", 0}}) {
    uint64_t next = 1;
    for (const auto& [name, id] : bindings) next = std::max(next, uint64_t(id) + 1);
    std::set<std::string> names;
    world.query<const Net>().each([&](const Net& net) { names.insert(net.values.begin(), net.values.end()); });
    for (const auto& name : names) if (!bindings.contains(name)) {
        if (next > UINT32_MAX) throw Error("Net id space exhausted");
        bindings[name] = uint32_t(next++);
    }
    std::vector<std::pair<Entity, Pins>> jobs;
    world.query<const Net, Entity>().each([&](const Net& net, Entity e) {
        Pins pins;
        for (const auto& name : net.values) pins.values.push_back(bindings.at(name));
        jobs.emplace_back(e, std::move(pins));
    });
    for (auto& [e, pins] : jobs) world.entity(e).add(std::move(pins));
    return bindings;
}
}
