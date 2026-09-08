#pragma once
#include "elysia/archive/registry.hpp"
#include "elysia/hierarchy.hpp"
#include "format.hpp"
#include <memory>
#include <set>

namespace elysia::prefab {
struct LookupScope {
    std::string current;
    std::vector<std::string> imports;
    std::map<std::string, std::string> aliases;
};

struct Local {
    std::string value;
};
struct NameTag {
    std::string value;
};
struct Refs {
    std::map<std::string, std::string> values;
};
struct Bound {
    std::map<std::string, Entity> values;
};
using ::elysia::ChildOf;
using ::elysia::Children;
struct PrefabEntityId {
    Entity instance;
    uint32_t local;
};
struct Net {
    std::vector<std::string> values;
};
struct Pins {
    std::vector<uint32_t> values;
};
struct Use {
    std::string prefab;
    Object params;
};
struct PrefabRoot {
    std::string name;
    Object params;
};

class ComponentRegistry {
  public:
    // Passing an existing registry borrows it. Copies of this view share registrations.
    explicit ComponentRegistry(archive::SnapshotRegistry &registry) : archive_(&registry) {
        register_builtins();
    }

    // A standalone registry keeps its storage alive across copied views.
    ComponentRegistry() : ComponentRegistry(archive::SnapshotRegistry{}) {}
    explicit ComponentRegistry(archive::SnapshotRegistry &&registry)
        : owned_(std::make_shared<archive::SnapshotRegistry>(std::move(registry))), archive_(owned_.get()) {
        register_builtins();
    }

    template <class T> void register_type(std::string name = std::string(TypeTraits<T>::name())) {
        ensure_new<T>(name);
        archive_->register_type<T>(name);
    }

    template <class T, class Proxy> void register_proxy(std::string name) {
        ensure_new<T>(name);
        archive_->register_type_with_proxy<T, Proxy>(name);
    }

    const archive::ComponentFactory &resolve(std::string_view name, const LookupScope &scope = {}) const {
        if (name.starts_with("::")) {
            return require_name(name.substr(2));
        }
        if (name.find("::") != name.npos) {
            return require_name(name);
        }
        if (!scope.current.empty()) {
            if (auto *factory = find_name(detail::qualify(scope.current, name))) {
                return *factory;
            }
        }
        if (auto alias = scope.aliases.find(std::string(name)); alias != scope.aliases.end()) {
            return require_name(alias->second);
        }

        const archive::ComponentFactory *imported = nullptr;
        for (const auto &ns : scope.imports) {
            auto *candidate = find_name(detail::qualify(ns, name));
            if (!candidate)
                continue;
            if (imported && imported != candidate) {
                throw Error("Ambiguous registered name '" + std::string(name) + "': " + imported->key + " " +
                            candidate->key);
            }
            imported = candidate;
        }
        if (imported)
            return *imported;
        return require_name(name);
    }

    archive::SnapshotRegistry &archive_registry() { return *archive_; }
    const archive::SnapshotRegistry &archive_registry() const { return *archive_; }

    void decode(World &world, Entity e, const archive::ComponentFactory &fac, const Value &value) const {
        if (fac.type_id == TypeTraits<ChildOf>::id || fac.type_id == TypeTraits<Children>::id)
            throw Error("Hierarchy components must be expressed through record parent links");
        if (!fac.generic)
            throw Error("Component '" + fac.key + "' has no generic decoder");
        auto result = fac.generic->from_generic(world, e, value);
        if (result.is_err())
            throw Error("Component '" + fac.key + "': invalid data " + reflect::write_json(value));
    }

  private:
    // Read the authoritative factories: external registration/replacement is visible
    // immediately, including duplicate names that the archive itself permits.
    const archive::ComponentFactory *find_name(std::string_view name) const {
        const archive::ComponentFactory *match = nullptr;
        for (const auto &[id, factory] : archive_->factories()) {
            if (factory.key != name)
                continue;
            if (match)
                throw Error("Conflicting registered name '" + std::string(name) + "'");
            match = &factory;
        }
        return match;
    }

    const archive::ComponentFactory &require_name(std::string_view name) const {
        if (auto *factory = find_name(name))
            return *factory;
        throw Error("Unknown registered name '" + std::string(name) + "'");
    }

    template <class T> void ensure_new(const std::string &name) const {
        if (name.empty())
            throw Error("Empty registered name");
        if (archive_->factories().contains(TypeTraits<T>::id)) {
            throw Error("Native type already registered: '" + name + "'");
        }
        if (find_name(name))
            throw Error("Conflicting registered name '" + name + "'");
    }

    void register_builtins() {
        // Validate caller-supplied names before adding prefab codecs.
        std::set<std::string> names;
        for (const auto &[id, factory] : archive_->factories()) {
            if (factory.key.empty())
                throw Error("Empty registered name");
            if (!names.insert(factory.key).second) {
                throw Error("Conflicting registered name '" + factory.key + "'");
            }
        }
        scalar<Local, std::string>("Local");
        scalar<NameTag, std::string>("NameTag");
        scalar<Refs, std::map<std::string, std::string>>("Refs");
        scalar<Net, std::vector<std::string>>("Net");
        scalar<Pins, std::vector<uint32_t>>("Pins");
    }

    // Rust serde newtypes encode as their wrapped value, not {"value": ...}.
    template <class T, class Wire> void scalar(const std::string &name) {
        if (archive_->factories().contains(TypeTraits<T>::id))
            return;
        ensure_new<T>(name);
        archive::ComponentFactory fac{};
        fac.type_id = TypeTraits<T>::id;
        fac.key = name;
        fac.size = sizeof(T);
        fac.info = get_type_info_ptr<T>();
        archive::GenericCodec codec;
        codec.to_generic = [](const void *ptr) {
            const auto &[value] = *static_cast<const T *>(ptr);
            return parse_json(reflect::write_json(value));
        };
        codec.from_generic = [](World &world, Entity e, const Value &value) {
            auto parsed = reflect::read_json<Wire>(reflect::write_json(value));
            if (!parsed)
                return Result<void>::err(ErrorCode::InvalidOperation, "Invalid prefab builtin");
            world.entity(e).add(T{std::move(*parsed)});
            return Result<void>::ok();
        };
        codec.from_generic_cmd = [](CommandBuffer &cmd, Entity e, const Value &value) {
            auto parsed = reflect::read_json<Wire>(reflect::write_json(value));
            if (!parsed)
                return Result<void>::err(ErrorCode::InvalidOperation, "Invalid prefab builtin");
            cmd.insert(e, T{std::move(*parsed)});
            return Result<void>::ok();
        };
        fac.generic = std::move(codec);
        archive_->register_type_factory_direct(std::move(fac));
    }
    std::shared_ptr<archive::SnapshotRegistry> owned_;
    archive::SnapshotRegistry *archive_;
};
} // namespace elysia::prefab
