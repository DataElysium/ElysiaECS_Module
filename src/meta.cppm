module;
#include <string_view>
#include <type_traits>
#include <cstdint>
#include <concepts>
#include <unordered_map>
#include <algorithm>
#include <tuple>
#include <nameof.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

export module elysia.meta;
import elysia.core;

export namespace elysia {

// --- 1. Administrative Lists ---
template<typename... Ts> struct TypeList {
    using tuple_type = std::tuple<Ts...>;
    static constexpr size_t count = sizeof...(Ts);
};
template<typename... Ts> struct FilterList : TypeList<Ts...> {};
template<typename... Ts> struct ResourceList : TypeList<Ts...> {};

template<typename T> struct With { using type = T; };
template<typename T> struct Without { using type = T; };
template<typename T> struct Include { using type = T; };
struct IncludeInactive {};
struct IncludeAll {};
template<typename T> struct Res { 
    using type = T; 
    T& value;
    Res(T& v) : value(v) {}
    T* operator->() const { return &value; }
    operator T&() const { return value; }
};

// --- 2. Advanced Traits ---
template<typename T> struct is_with : std::false_type {};
template<typename T> struct is_with<With<T>> : std::true_type {};
template<typename T> inline constexpr bool is_with_v = is_with<T>::value;

template<typename T> struct is_without : std::false_type {};
template<typename T> struct is_without<Without<T>> : std::true_type {};
template<typename T> inline constexpr bool is_without_v = is_without<T>::value;

template<typename T> struct is_include : std::false_type {};
template<typename T> struct is_include<Include<T>> : std::true_type {};
template<typename T> inline constexpr bool is_include_v = is_include<T>::value;

template<typename T>
inline constexpr bool is_include_inactive_v =
    std::is_same_v<std::remove_cvref_t<T>, IncludeInactive>;

template<typename T>
inline constexpr bool is_include_all_v =
    std::is_same_v<std::remove_cvref_t<T>, IncludeAll>;

template<typename T> struct is_res_handle : std::false_type {};
template<typename T> struct is_res_handle<Res<T>> : std::true_type {};

template<typename T> 
inline constexpr bool is_res_handle_v = is_res_handle<std::remove_cvref_t<T>>::value;

template<typename T> struct GetRaw { using type = std::remove_cvref_t<T>; };
template<typename T> struct GetRaw<With<T>> { using type = std::remove_cvref_t<T>; };
template<typename T> struct GetRaw<Without<T>> { using type = std::remove_cvref_t<T>; };
template<typename T> struct GetRaw<Include<T>> { using type = std::remove_cvref_t<T>; };
template<typename T> struct GetRaw<Res<T>> { using type = std::remove_cvref_t<T>; };
template<typename T> using GetRawT = typename GetRaw<T>::type;

template <typename...> inline constexpr bool is_unique_v = true;
template <typename T, typename... Rest>
inline constexpr bool is_unique_v<T, Rest...> = (!std::is_same_v<T, Rest> && ...) && is_unique_v<Rest...>;

struct DisabledTag {};

// --- 3. Component Meta Logic ---
struct TypeHooks {
    void(*ctor)(void*) = nullptr;
    void(*dtor)(void*) = nullptr;
    void(*move)(void* dst, void* src) = nullptr;
};

template<typename T>
constexpr TypeHooks make_type_hooks() {
    TypeHooks h;
    using RawT = std::remove_cvref_t<T>;
    
    if constexpr (std::is_default_constructible_v<RawT>) {
        h.ctor = [](void* p) { 
            new (p) RawT(); 
        };
    }

    if constexpr (!std::is_trivially_destructible_v<RawT>) {
        h.dtor = [](void* p) { 
            RawT* obj = static_cast<RawT*>(p); 
            obj->~RawT(); 
        };
    }
    
    if constexpr (!std::is_trivially_move_constructible_v<RawT>) 
        h.move = [](void* d, void* s) { new (d) RawT(std::move(*static_cast<RawT*>(s))); };
        
    return h;
}

constexpr uint64_t fnv1a_64(std::string_view str) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : str) { hash ^= static_cast<uint64_t>(c); hash *= 0x100000001b3ULL; }
    return hash;
}

template<typename T>
struct TypeTraits {
    static constexpr std::string_view name() {
        if constexpr (requires { { T::elysia_name } -> std::convertible_to<std::string_view>; }) return T::elysia_name;
        else return nameof::nameof_type<T>();
    }
    static constexpr uint64_t id = fnv1a_64(name());
};

struct TypeInfo { uint64_t id; std::string_view name; size_t size; size_t alignment; TypeHooks hooks; };

template<typename T> constexpr TypeInfo make_type_info() {
    using RawT = std::remove_cvref_t<T>;
    constexpr size_t size = std::is_empty_v<RawT> ? 0 : sizeof(RawT);
    return TypeInfo{ .id = TypeTraits<RawT>::id, .name = TypeTraits<RawT>::name(), .size = size, .alignment = alignof(RawT), .hooks = make_type_hooks<RawT>() };
}
template<typename T> const TypeInfo* get_type_info_ptr() { static constexpr TypeInfo info = make_type_info<T>(); return &info; }

struct MetaConfig { static constexpr uint32_t INVALID_LOCAL_ID = 0xFFFFFFFF; };

class ComponentRegistry {
public:
    ComponentRegistry() = default;
    
    // Non-copyable due to unique_ptr
    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;
    ComponentRegistry(ComponentRegistry&&) = default;
    ComponentRegistry& operator=(ComponentRegistry&&) = default;

    ~ComponentRegistry();
    
    // ⚠️ THREAD-SAFETY: next_local_index_++ and id_to_local_/id_to_info_ inserts
    // are not atomic. Concurrent first-time registration of different types
    // from parallel systems will cause data races.
    // 👉 CONSTRAINT: All component types must be registered before entering
    //    parallel execution. The find() fast-path (line 137) is read-only and
    //    safe. 99.9% of calls hit this cache.
    // 👉 FUTURE: Consider lock-free registration or enforce universal
    //    pre-registration via Scheduler::build() phase.
    inline uint32_t ensure_registered(const TypeInfo* info) {
        if (!info) return MetaConfig::INVALID_LOCAL_ID;
        
        auto it = id_to_local_.find(info->id);
        if (it != id_to_local_.end()) {
            // 🌸 ABI Stability Check
            const auto* existing = id_to_info_[info->id];
            if (existing->size != info->size) {
                std::string msg = "Elysia ABI Conflict! Component '";
                msg += std::string(info->name);
                msg += "' (ID: " + std::to_string(info->id);
                msg += ") was previously registered with size " + std::to_string(existing->size);
                msg += " but is now being used with size " + std::to_string(info->size);
                msg += ". This usually happens when the same component name is defined differently across TUs.";
                throw std::runtime_error(msg);
            }
            return it->second;
        }
        
        uint32_t local = next_local_index_++;
        id_to_local_[info->id] = local; 
        id_to_info_[info->id] = info;
        return local;
    }

    /**
     * @brief Registers a type without having its C++ definition. 
     * Returns a mutable pointer to allow post-registration hook injection.
     */
    inline TypeInfo* register_opaque(uint64_t id, std::string_view name, size_t size, size_t alignment) {
        if (auto it = id_to_info_.find(id); it != id_to_info_.end()) {
            // Find the original mutable pointer in owned_infos_ if possible
            for (auto& info : owned_infos_) if (info->id == id) return info.get();
            return nullptr; // It was a static constexpr type, cannot mutate
        }

        auto owned_info = std::make_unique<TypeInfo>();
        owned_info->id = id;
        owned_info->size = size;
        owned_info->alignment = alignment;
        
        auto& owned_name = owned_names_.emplace_back(std::make_unique<std::string>(name));
        owned_info->name = *owned_name;
        owned_info->hooks = TypeHooks{}; 

        TypeInfo* ptr = owned_info.get();
        id_to_info_[id] = ptr; // Map to the new info
        id_to_local_[id] = next_local_index_++;
        owned_infos_.push_back(std::move(owned_info));
        
        return ptr;
    }

    inline uint32_t local_index(uint64_t id) const {
        auto it = id_to_local_.find(id);
        return (it != id_to_local_.end()) ? it->second : MetaConfig::INVALID_LOCAL_ID;
    }
    inline const TypeInfo* get_info(uint64_t id) const {
        auto it = id_to_info_.find(id);
        return (it != id_to_info_.end()) ? it->second : nullptr;
    }
private:
    std::unordered_map<uint64_t, uint32_t> id_to_local_;
    std::unordered_map<uint64_t, const TypeInfo*> id_to_info_;
    std::vector<std::unique_ptr<TypeInfo>> owned_infos_;
    std::vector<std::unique_ptr<std::string>> owned_names_;
    uint32_t next_local_index_ = 0;
};

// --- 4. Function Traits ---
template<typename T> struct function_traits;
template<typename R, typename... Args> struct function_traits<R(*)(Args...)> { using args_tuple = std::tuple<Args...>; };
template<typename F> struct function_traits : function_traits<decltype(&F::operator())> {};
template<typename C, typename R, typename... Args> struct function_traits<R (C::*)(Args...) const> { using args_tuple = std::tuple<Args...>; };
template<typename C, typename R, typename... Args> struct function_traits<R (C::*)(Args...)> { using args_tuple = std::tuple<Args...>; };
template<typename F> using args_tuple_t = typename function_traits<std::decay_t<F>>::args_tuple;

} // namespace elysia
