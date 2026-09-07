#pragma once
#include "registry.hpp"
#include "substitute.hpp"
#include <limits>

namespace elysia::prefab {
struct Spawned {
    std::vector<Entity> roots;
    std::vector<Entity> entities;
};

class PrefabRegistry {
public:
    explicit PrefabRegistry(ComponentRegistry components = ComponentRegistry{}) : components_(std::move(components)) {}
    const ComponentRegistry& components() const { return components_; }
    void register_global(std::string name, Value value) { globals_[std::move(name)] = std::move(value); }
    void component_scope(LookupScope scope) { scope_ = std::move(scope); }
    void register_net(std::string name) { net_names_.insert(std::move(name)); }
    const World& template_world() const { return *templates_; }

    void load_library(Library lib) {
        // Build privately: a failed replacement preserves the previous library.
        auto world = std::make_unique<World>();
        for (const auto& [name, cls] : lib) {
            validate_records(name, cls);
            auto root = world->spawn().add(PrefabRoot{name, cls.params}).entity;
            std::map<uint32_t, Entity> entities;
            for (const auto& row : cls.body) entities[row.id] = world->spawn().entity;
            for (const auto& row : cls.body) {
                auto entity = entities.at(row.id);
                world->entity(entity).add(PrefabEntityId{root, row.id});
                world->entity(entity).add(ChildOf{row.parent ? entities.at(*row.parent) : root});
                for (const auto& [key, value] : row.components) {
                    if (key == "Use") {
                        const auto& use = detail::object(value);
                        detail::fields(use, {"prefab", "params"});
                        Object inputs;
                        if (auto* p = detail::find(use, "params")) inputs = detail::object(*p);
                        world->entity(entity).add(Use{detail::string(detail::required(use, "prefab")), std::move(inputs)});
                        continue;
                    }
                    auto& fac = components_.resolve(key, scope_for(name));
                    // Authored expressions stay in lib; materialize defaults where possible.
                    try {
                        auto defaults = detail::object(detail::globals(Value(cls.params), globals_));
                        components_.decode(*world, entity, fac, detail::substitute(value, defaults, globals_));
                    } catch (const Error& error) {
                        if (!detail::has_param(value)) throw Error("Prefab '" + name + "' record " + std::to_string(row.id) + ": " + error.what());
                    }
                }
            }
        }
        library_ = std::move(lib);
        templates_ = std::move(world);
    }
    Library export_library() const { return library_; }
    PrefabClass export_class(const std::string& name) const { return library_.at(resolve_class(name, "")); }

    Spawned spawn_class(World& world, const std::string& name, const Object& params = {},
                        const std::string& prefix = "") const {
        std::vector<Pending> pending;
        std::vector<std::string> stack;
        size_t instance = 0;
        expand(resolve_class(name, ""), params, prefix, {}, pending, stack, instance);
        // Decode into a private world first. Invalid component data never reaches the target.
        World validation;
        for (const auto& row : pending) {
            auto e = validation.spawn().entity;
            decode_record(validation, e, row);
        }
        Spawned result;
        try {
            for (size_t i = 0; i < pending.size(); ++i) result.entities.push_back(world.spawn().entity);
            std::map<size_t, Entity> instances;
            for (size_t i = 0; i < pending.size(); ++i) instances.try_emplace(pending[i].instance, result.entities[i]);
            for (size_t i = 0; i < pending.size(); ++i) {
                const auto& row = pending[i];
                auto e = result.entities[i];
                decode_record(world, e, row);
                world.entity(e).add(PrefabEntityId{instances.at(row.instance), row.local});
                if (row.parent) world.entity(e).add(ChildOf{result.entities.at(*row.parent)});
                else result.roots.push_back(e);
            }
        } catch (...) {
            for (auto e : result.entities) world.despawn(e);
            throw;
        }
        return result;
    }

private:
    struct Pending {
        uint32_t local;
        size_t instance;
        std::optional<size_t> parent;
        std::string context;
        std::vector<std::pair<const archive::ComponentFactory*, Value>> components;
    };
    LookupScope scope_for(const std::string& cls) const {
        auto scope = scope_;
        if (scope.current.empty()) scope.current = detail::scope_of(cls);
        return scope;
    }
    std::string resolve_class(const std::string& name, const std::string& from) const {
        if (name.starts_with("::")) return resolve_class(name.substr(2), "");
        if (name.find("::") == name.npos) {
            auto local = detail::qualify(detail::scope_of(from), name);
            if (library_.contains(local)) return local;
        }
        if (library_.contains(name)) return name;
        throw Error("Unknown prefab '" + name + "' in '" + from + "'");
    }
    static void validate_records(const std::string& name, const PrefabClass& cls) {
        std::map<uint32_t, std::optional<uint32_t>> parents;
        for (const auto& row : cls.body)
            if (!parents.emplace(row.id, row.parent).second) throw Error("Prefab '" + name + "': duplicate record id " + std::to_string(row.id));
        for (const auto& row : cls.body) {
            std::set<uint32_t> path{row.id};
            auto parent = row.parent;
            while (parent) {
                if (!parents.contains(*parent)) throw Error("Prefab '" + name + "': missing parent " + std::to_string(*parent));
                if (!path.insert(*parent).second) throw Error("Prefab '" + name + "': cyclic parent hierarchy");
                parent = parents.at(*parent);
            }
        }
    }
    void decode_record(World& world, Entity e, const Pending& row) const {
        try { for (const auto& [fac, value] : row.components) components_.decode(world, e, *fac, value); }
        catch (const Error& error) { throw Error(row.context + ": " + error.what()); }
    }
    // Only literal net names are scoped. Parameter/global values are caller bindings.
    static Value qualify_nets(const Value& value, const std::string& prefix) {
        if (auto* s = std::get_if<std::string>(&value.get())) {
            if (s->starts_with('$') || s->starts_with('@')) return value;
            return Value(prefix + *s);
        }
        if (auto* a = std::get_if<Array>(&value.get())) {
            Array out; for (const auto& v : *a) out.push_back(qualify_nets(v, prefix)); return out;
        }
        if (auto* o = std::get_if<Object>(&value.get())) {
            Object out; for (const auto& [k,v] : *o) out[k] = qualify_nets(v, prefix); return out;
        }
        return value;
    }
    std::set<std::string> net_params(const std::string& name, std::set<std::string>& visiting) const {
        if (!visiting.insert(name).second) throw Error("Recursive prefab composition at '" + name + "'");
        std::set<std::string> result;
        for (const auto& row : library_.at(name).body) for (const auto& [key, value] : row.components) {
            if (net_names_.contains(key)) detail::param_refs(value, result);
            else if (key == "Use") {
                const auto& use = detail::object(value);
                auto child = resolve_class(detail::string(detail::required(use, "prefab")), name);
                auto inputs = net_params(child, visiting);
                if (auto* args = detail::find(use, "params"))
                    for (const auto& input : inputs)
                        if (auto* argument = detail::param(detail::object(*args), input)) detail::param_refs(*argument, result);
            }
        }
        visiting.erase(name);
        return result;
    }
    void expand(const std::string& name, const Object& overrides, const std::string& prefix,
                std::optional<size_t> parent, std::vector<Pending>& pending,
                std::vector<std::string>& stack, size_t& next_instance) const {
        if (stack.size() >= 128 || std::find(stack.begin(), stack.end(), name) != stack.end())
            throw Error("Recursive prefab composition at '" + name + "'");
        stack.push_back(name);
        const auto& cls = library_.at(name);
        Value params(cls.params);
        for (const auto& [k, v] : overrides)
            if (!detail::find(cls.params, k)) throw Error("Prefab '" + name + "': unknown param '" + k + "'");
        detail::merge(params, Value(overrides));
        params = detail::globals(params, globals_);
        detail::require_values(params, "");
        const auto instance = next_instance++;
        std::map<uint32_t, size_t> records;
        for (const auto& row : cls.body) {
            records[row.id] = pending.size();
            pending.push_back({row.id, instance, {}, "Prefab '" + name + "' record " + std::to_string(row.id), {}});
        }
        size_t use_index = 0;
        for (const auto& row : cls.body) {
            const auto index = records.at(row.id);
            pending[index].parent = row.parent ? std::optional<size_t>(records.at(*row.parent)) : parent;
            std::set<uint64_t> types;
            for (const auto& [key, original] : row.components) {
                auto authored = net_names_.contains(key) ? qualify_nets(original, prefix) : original;
                if (key == "Use" && !prefix.empty()) {
                    auto& use = std::get<Object>(authored.get());
                    auto child = resolve_class(detail::string(detail::required(use, "prefab")), name);
                    std::set<std::string> visiting;
                    auto inputs = net_params(child, visiting);
                    if (detail::find(use, "params")) {
                        auto& args = std::get<Object>(use["params"].get());
                        for (const auto& input : inputs) if (auto* arg = detail::param(args, input)) *arg = qualify_nets(*arg, prefix);
                    }
                }
                auto value = detail::substitute(authored, detail::object(params), globals_);
                if (key == "Use") {
                    const auto& use = detail::object(value);
                    detail::fields(use, {"prefab", "params"});
                    Object child_params;
                    if (auto* p = detail::find(use, "params")) child_params = detail::object(*p);
                    expand(resolve_class(detail::string(detail::required(use, "prefab")), name), child_params,
                           prefix + std::to_string(use_index++) + ".", index, pending, stack, next_instance);
                } else {
                    const auto& fac = components_.resolve(key, scope_for(name));
                    if (!types.insert(fac.type_id).second) throw Error(pending[index].context + ": component named twice: " + key);
                    pending[index].components.emplace_back(&fac, std::move(value));
                }
            }
        }
        stack.pop_back();
    }
    ComponentRegistry components_;
    Library library_;
    Object globals_;
    LookupScope scope_;
    std::set<std::string> net_names_{"Net"};
    std::unique_ptr<World> templates_ = std::make_unique<World>();
};
}
