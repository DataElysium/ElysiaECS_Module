#pragma once
#include "format.hpp"
#include <set>
#include <utility>

namespace elysia::prefab::detail {
inline bool reference(std::string_view s) {
    return s.starts_with("$ref:") || s.starts_with("@ref:") || s.starts_with("$id:");
}
inline const Value* param(const Object& params, std::string_view path) {
    if (auto* p = find(params, path)) return p;
    auto dot = path.find('.');
    auto* value = find(params, path.substr(0, dot));
    while (value && dot != path.npos) {
        path.remove_prefix(dot + 1);
        dot = path.find('.');
        auto* obj = std::get_if<Object>(&value->get());
        value = obj ? find(*obj, path.substr(0, dot)) : nullptr;
    }
    return value;
}
inline Value* param(Object& params, std::string_view path) {
    return const_cast<Value*>(param(std::as_const(params), path));
}
inline void param_refs(const Value& value, std::set<std::string>& names) {
    if (auto* s = std::get_if<std::string>(&value.get()); s && s->starts_with('$') && !reference(*s))
        names.insert(s->substr(1));
    if (auto* a = std::get_if<Array>(&value.get())) for (const auto& v : *a) param_refs(v, names);
    if (auto* o = std::get_if<Object>(&value.get())) for (const auto& [k, v] : *o) param_refs(v, names);
}
inline Value globals(const Value& value, const Object& table, std::vector<std::string>& stack) {
    if (auto* s = std::get_if<std::string>(&value.get()); s && s->starts_with('@') && !reference(*s)) {
        auto name = s->substr(1);
        auto* target = find(table, name);
        if (!target) throw Error("Unknown global '@" + name + "'");
        if (std::find(stack.begin(), stack.end(), name) != stack.end()) throw Error("Cyclic global '@" + name + "'");
        stack.push_back(name);
        auto out = globals(*target, table, stack);
        stack.pop_back();
        return out;
    }
    if (auto* a = std::get_if<Array>(&value.get())) {
        Array out;
        for (const auto& v : *a) out.push_back(globals(v, table, stack));
        return out;
    }
    if (auto* o = std::get_if<Object>(&value.get())) {
        Object out;
        for (const auto& [k, v] : *o) out[k] = globals(v, table, stack);
        return out;
    }
    return value;
}
inline Value globals(const Value& value, const Object& table) {
    std::vector<std::string> stack;
    return globals(value, table, stack);
}
inline void require_values(const Value& value, const std::string& path) {
    if (value.is_null()) throw Error("Required param '" + path + "' was not provided");
    if (auto* o = std::get_if<Object>(&value.get()))
        for (const auto& [k, v] : *o) require_values(v, path.empty() ? k : path + "." + k);
    if (auto* a = std::get_if<Array>(&value.get()))
        for (size_t i = 0; i < a->size(); ++i) require_values((*a)[i], path + "[" + std::to_string(i) + "]");
}
inline Value substitute(const Value& value, const Object& params, const Object& global_table) {
    if (auto* s = std::get_if<std::string>(&value.get())) {
        if (reference(*s)) return value;
        if (s->starts_with('$')) {
            auto* v = param(params, s->substr(1));
            if (!v) throw Error("Unknown param '" + *s + "'");
            return *v;
        }
        if (s->starts_with('@')) return globals(value, global_table);
    }
    if (auto* a = std::get_if<Array>(&value.get())) {
        Array out;
        for (const auto& v : *a) out.push_back(substitute(v, params, global_table));
        return out;
    }
    if (auto* o = std::get_if<Object>(&value.get())) {
        Object out;
        for (const auto& [k, v] : *o) out[k] = substitute(v, params, global_table);
        return out;
    }
    return value;
}
inline bool has_param(const Value& value) {
    if (auto* s = std::get_if<std::string>(&value.get())) return s->starts_with('$') && !reference(*s);
    if (auto* a = std::get_if<Array>(&value.get())) for (const auto& v : *a) if (has_param(v)) return true;
    if (auto* o = std::get_if<Object>(&value.get())) for (const auto& [k, v] : *o) if (has_param(v)) return true;
    return false;
}
}
