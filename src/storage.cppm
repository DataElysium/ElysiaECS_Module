module;
#include <vector>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <span>

export module elysia.storage;

import elysia.config;
import elysia.meta;
import elysia.entity;
import elysia.table;
import elysia.mem;
import elysia.bitset;
import elysia.core;

export namespace elysia {

/**
 * @brief Archetype represents a group of entities with the same component types.
 * Now provides a richer API for low-level data access.
 */
template<typename Config = DefaultConfig>
class Archetype {
public:
    Archetype(std::span<const TypeInfo* const> sorted_types, Allocator* alloc, uint32_t id, ComponentRegistry* reg = nullptr)
        : id_(id)
        , table_(sorted_types, alloc)
    {
        types_.assign(sorted_types.begin(), sorted_types.end());
        
        if (reg) {
            for (const auto* t : types_) {
                uint32_t local_idx = reg->ensure_registered(t);
                bit_sig_.set(local_idx);
            }
        }
    }

    uint32_t id() const { return id_; }
    uint64_t hash() const { return bit_sig_.hash_value(); }
    const SignatureBuffer<>& bit_sig() const { return bit_sig_; }
    std::span<const TypeInfo* const> types() const { return types_; }

    /**
     * @brief Get the column index for a specific component type.
     * Uses linear search - highly efficient for small component sets due to cache locality.
     */
    std::optional<size_t> get_column_index(uint64_t type_id) const {
        for (size_t i = 0; i < types_.size(); ++i) {
            if (types_[i]->id == type_id) return i;
        }
        return std::nullopt;
    }

    /**
     * @brief Strong-typed column lookup.
     */
    template<typename T>
    std::optional<size_t> get_column() const {
        return get_column_index(TypeTraits<std::remove_cvref_t<T>>::id);
    }

    /**
     * @brief Check if this archetype contains a component.
     */
    template<typename T>
    bool has() const {
        return get_column<T>().has_value();
    }

    struct Modifier {
        Archetype* self;
        size_t push(Entity e) { return self->table_.push(e); }
        size_t push_raw(Entity e) { return self->table_.push_raw(e); }
        size_t push_batch_raw(size_t count) { return self->table_.push_batch_raw(count); }
        std::optional<Entity> swap_remove(size_t row) { return self->table_.swap_remove(row); }
        void clear() { self->table_.clear(); }
    };

    Modifier modify() { return {this}; }
    [[nodiscard]] size_t count() const { return table_.count(); }

    Table<Config>& table() { return table_; }

private:
    uint32_t id_;
    SignatureBuffer<> bit_sig_;
    std::vector<const TypeInfo*> types_;
    Table<Config> table_;
};

} // namespace elysia