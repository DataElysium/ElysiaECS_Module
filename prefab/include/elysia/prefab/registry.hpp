#pragma once
#include "format.hpp"
#include "elysia/archive/registry.hpp"
#include <set>

namespace elysia::prefab {
struct LookupScope {
    std::string current;
    std::vector<std::string> imports;
    std::map<std::string, std::string> aliases;
};

// These names are authoring symbols, not compiler namespaces or native hashes.
class NameTable {
public:
    void add(std::string name, uint64_t id) {
        if (name.empty()) throw Error("Empty registered name");
        auto [it, inserted] = names_.emplace(std::move(name), id);
        if (!inserted && it->second != id) throw Error("Conflicting registered name '" + it->first + "'");
    }
    std::string resolve(std::string_view name, const LookupScope& scope = {}) const {
        auto exact = [&](std::string key) -> std::string {
            if (!names_.contains(key)) throw Error("Unknown registered name '" + key + "'");
            return key;
        };
        if (name.starts_with("::")) return exact(std::string(name.substr(2)));
        if (name.find("::") != name.npos) return exact(std::string(name));
        auto local = detail::qualify(scope.current, name);
        if (!scope.current.empty() && names_.contains(local)) return local;
        if (auto it = scope.aliases.find(std::string(name)); it != scope.aliases.end()) return exact(it->second);
        std::set<std::string> candidates;
        for (const auto& ns : scope.imports) {
            auto key = detail::qualify(ns, name);
            if (names_.contains(key)) candidates.insert(std::move(key));
        }
        if (candidates.size() > 1) {
            std::string message = "Ambiguous registered name '" + std::string(name) + "':";
            for (const auto& candidate : candidates) message += " " + candidate;
            throw Error(message);
        }
        if (!candidates.empty()) return *candidates.begin();
        return exact(std::string(name));
    }
    uint64_t id(std::string_view name, const LookupScope& scope = {}) const { return names_.at(resolve(name, scope)); }
private:
    std::map<std::string, uint64_t> names_;
};

struct Local { std::string value; };
struct NameTag { std::string value; };
struct Refs { std::map<std::string, std::string> values; };
struct Bound { std::map<std::string, Entity> values; };
struct ChildOf { Entity parent; };
struct PrefabEntityId { Entity instance; uint32_t local; };
struct Net { std::vector<std::string> values; };
struct Pins { std::vector<uint32_t> values; };
struct Use { std::string prefab; Object params; };
struct PrefabRoot { std::string name; Object params; };

class ComponentRegistry {
public:
    explicit ComponentRegistry(archive::SnapshotRegistry registry = {}) : archive_(std::move(registry)) {
        for (const auto& [id, fac] : archive_.factories()) names_.add(fac.key, id);
        scalar<Local, std::string>("Local");
        scalar<NameTag, std::string>("NameTag");
        scalar<Refs, std::map<std::string, std::string>>("Refs");
        scalar<Net, std::vector<std::string>>("Net");
        scalar<Pins, std::vector<uint32_t>>("Pins");
    }
    template<class T> void register_type(std::string name = std::string(TypeTraits<T>::name())) {
        ensure_new<T>(name);
        archive_.register_type<T>(name);
        names_.add(std::move(name), TypeTraits<T>::id);
    }
    template<class T, class Proxy> void register_proxy(std::string name) {
        ensure_new<T>(name);
        archive_.register_type_with_proxy<T, Proxy>(name);
        names_.add(std::move(name), TypeTraits<T>::id);
    }
    const archive::ComponentFactory& resolve(std::string_view name, const LookupScope& scope = {}) const {
        return archive_.factories().at(names_.id(name, scope));
    }
    const archive::SnapshotRegistry& archive_registry() const { return archive_; }
    const NameTable& names() const { return names_; }
    void decode(World& world, Entity e, const archive::ComponentFactory& fac, const Value& value) const {
        if (!fac.generic) throw Error("Component '" + fac.key + "' has no generic decoder");
        auto result = fac.generic->from_generic(world, e, value);
        if (result.is_err()) throw Error("Component '" + fac.key + "': invalid data " + reflect::write_json(value));
    }
private:
    template<class T> void ensure_new(const std::string& name) const {
        if (archive_.factories().contains(TypeTraits<T>::id)) throw Error("Native type already registered: '" + name + "'");
        for (const auto& [id, fac] : archive_.factories())
            if (fac.key == name) throw Error("Conflicting registered name '" + name + "'");
    }
    // Rust serde newtypes encode as their wrapped value, not {"value": ...}.
    template<class T, class Wire> void scalar(const std::string& name) {
        if (archive_.factories().contains(TypeTraits<T>::id)) return;
        ensure_new<T>(name);
        archive::ComponentFactory fac{};
        fac.type_id = TypeTraits<T>::id; fac.key = name;
        fac.size = sizeof(T); fac.info = get_type_info_ptr<T>();
        archive::GenericCodec codec;
        codec.to_generic = [](const void* ptr) {
            const auto& [value] = *static_cast<const T*>(ptr);
            return parse_json(reflect::write_json(value));
        };
        codec.from_generic = [](World& world, Entity e, const Value& value) {
            auto parsed = reflect::read_json<Wire>(reflect::write_json(value));
            if (!parsed) return Result<void>::err(ErrorCode::InvalidOperation, "Invalid prefab builtin");
            world.entity(e).add(T{std::move(*parsed)});
            return Result<void>::ok();
        };
        codec.from_generic_cmd = [](CommandBuffer& cmd, Entity e, const Value& value) {
            auto parsed = reflect::read_json<Wire>(reflect::write_json(value));
            if (!parsed) return Result<void>::err(ErrorCode::InvalidOperation, "Invalid prefab builtin");
            cmd.insert(e, T{std::move(*parsed)});
            return Result<void>::ok();
        };
        fac.generic = std::move(codec);
        archive_.register_type_factory_direct(std::move(fac));
        names_.add(name, TypeTraits<T>::id);
    }
    archive::SnapshotRegistry archive_;
    NameTable names_;
};
}
