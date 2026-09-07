#pragma once
#include "elysia/reflect_wrapper.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace elysia::prefab {
using Value = reflect::Generic;
using Object = Value::Object;
using Array = Value::Array;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {
inline const Value* find(const Object& object, std::string_view key) {
    for (const auto& [k, v] : object) if (k == key) return &v;
    return nullptr;
}
inline const Object& object(const Value& value) {
    if (auto* p = std::get_if<Object>(&value.get())) return *p;
    throw Error("Expected an object");
}
inline const Array& array(const Value& value) {
    if (auto* p = std::get_if<Array>(&value.get())) return *p;
    throw Error("Expected an array");
}
inline std::string string(const Value& value) {
    if (auto* p = std::get_if<std::string>(&value.get())) return *p;
    throw Error("Expected a string");
}
inline const Value& required(const Object& obj, std::string_view key) {
    if (auto* p = find(obj, key)) return *p;
    throw Error("Missing field '" + std::string(key) + "'");
}
inline uint32_t id(const Value& value) {
    auto* p = std::get_if<int64_t>(&value.get());
    if (!p || *p < 0 || uint64_t(*p) > UINT32_MAX) throw Error("Expected a uint32 record id");
    return uint32_t(*p);
}
inline void fields(const Object& obj, std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, v] : obj)
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw Error("Unknown field '" + key + "'");
}
inline std::string scope_of(std::string_view name) {
    auto i = name.rfind("::");
    return i == name.npos ? "" : std::string(name.substr(0, i));
}
inline std::string qualify(std::string_view scope, std::string_view name) {
    return scope.empty() ? std::string(name) : std::string(scope) + "::" + std::string(name);
}
inline void merge(Value& dst, const Value& patch) {
    auto* d = std::get_if<Object>(&dst.get());
    auto* p = std::get_if<Object>(&patch.get());
    if (d && p) { for (const auto& [k, v] : *p) merge((*d)[k], v); }
    else dst = patch;
}
}

inline Value parse_json(const std::string& text) {
    auto parsed = reflect::read_json<Value>(text);
    if (!parsed) throw Error("Invalid prefab JSON: " + parsed.error().what());
    return std::move(*parsed);
}

struct EntityRecord {
    uint32_t id;
    std::optional<uint32_t> parent;
    Object components;
};
struct PrefabClass { Object params; std::vector<EntityRecord> body; };
using Library = std::map<std::string, PrefabClass>;
struct Instance { std::string id; std::string prefab; Object params; };
struct Document { Library prefabs; std::vector<Instance> instances; };

inline Library read_library(const Value& value) {
    Library out;
    for (const auto& [name, data] : detail::object(value)) {
        try {
            const auto& obj = detail::object(data);
            detail::fields(obj, {"params", "body"});
            PrefabClass cls;
            if (auto* p = detail::find(obj, "params")) cls.params = detail::object(*p);
            for (const auto& row : detail::array(detail::required(obj, "body"))) {
                const auto& record = detail::object(row);
                detail::fields(record, {"id", "parent", "components"});
                EntityRecord r{detail::id(detail::required(record, "id")), {},
                               detail::object(detail::required(record, "components"))};
                if (auto* p = detail::find(record, "parent"); p && !p->is_null()) r.parent = detail::id(*p);
                cls.body.push_back(std::move(r));
            }
            if (!out.emplace(name, std::move(cls)).second) throw Error("Duplicate prefab name");
        } catch (const Error& e) { throw Error("Prefab '" + name + "': " + e.what()); }
    }
    return out;
}
inline Value write_library(const Library& lib) {
    Object result;
    for (const auto& [name, cls] : lib) {
        Array body;
        for (const auto& r : cls.body) {
            Object row;
            row["id"] = int64_t(r.id);
            if (r.parent) row["parent"] = int64_t(*r.parent);
            row["components"] = r.components;
            body.emplace_back(std::move(row));
        }
        Object obj;
        obj["params"] = cls.params;
        obj["body"] = std::move(body);
        result[name] = std::move(obj);
    }
    return result;
}
inline Document read_document(const Value& value) {
    const auto& obj = detail::object(value);
    detail::fields(obj, {"prefabs", "instances"});
    Document result{read_library(detail::required(obj, "prefabs")), {}};
    std::map<std::string, bool> ids;
    for (const auto& row : detail::array(detail::required(obj, "instances"))) {
        const auto& instance = detail::object(row);
        detail::fields(instance, {"id", "prefab", "params"});
        Instance item{detail::string(detail::required(instance, "id")),
                      detail::string(detail::required(instance, "prefab")), {}};
        if (!ids.emplace(item.id, true).second) throw Error("Duplicate instance id '" + item.id + "'");
        if (auto* params = detail::find(instance, "params")) item.params = detail::object(*params);
        result.instances.push_back(std::move(item));
    }
    return result;
}
}
