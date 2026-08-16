module;
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory_resource>
#include <algorithm>
#include <cstring>
#include <bit>
#include <concepts>
#include <span>
#include <cassert>
#include <optional>
#include <memory>

export module elysia.table;

import elysia.config;
import elysia.meta;
import elysia.entity;
import elysia.mem;

export namespace elysia {

/**
 * @brief Chunk: Manages a physical block of SoA memory.
 * Now supports dynamic resizing for Monolithic mode.
 */
class ELYSIA_API Chunk {
public:
    struct Layout {
        size_t entity_offset;
        std::vector<size_t> component_offsets;
        size_t total_size;
        size_t capacity; 
    };

    static Layout calculate_layout(std::span<const TypeInfo* const> types, size_t capacity) {
        Layout layout;
        layout.capacity = capacity;
        
        size_t current_offset = 0;
        layout.entity_offset = current_offset;
        current_offset += capacity * sizeof(Entity);
        
        layout.component_offsets.clear();
        for (const auto* type : types) {
            current_offset = (current_offset + type->alignment - 1) & ~(type->alignment - 1);
            layout.component_offsets.push_back(current_offset);
            current_offset += capacity * type->size;
        }
        layout.total_size = (current_offset + 63) & ~63;
        return layout;
    }

    Chunk(size_t capacity, std::span<const TypeInfo* const> types, Allocator* alloc)
        : alloc_(alloc) 
    {
        types_.assign(types.begin(), types.end());
        layout_ = calculate_layout(types_, capacity);
        data_ = static_cast<std::byte*>(alloc_->allocate(layout_.total_size));
    }

    ~Chunk() {
        if (data_) {
            clear();
            alloc_->deallocate(data_, layout_.total_size);
        }
    }

    Chunk(Chunk&& other) noexcept 
        : data_(other.data_), count_(other.count_), layout_(std::move(other.layout_)), types_(std::move(other.types_)), alloc_(other.alloc_) 
    {
        other.data_ = nullptr;
        other.count_ = 0;
    }

    [[nodiscard]] size_t count() const { return count_; }
    [[nodiscard]] size_t capacity() const { return layout_.capacity; }
    [[nodiscard]] bool is_full() const { return count_ == layout_.capacity; }
    [[nodiscard]] bool is_empty() const { return count_ == 0; }
    [[nodiscard]] std::byte* data() { return data_; }

    Entity& entity(size_t index) {
        return *reinterpret_cast<Entity*>(data_ + layout_.entity_offset + index * sizeof(Entity));
    }

    void* __restrict component(size_t comp_idx, size_t index) {
        return data_ + layout_.component_offsets[comp_idx] + index * types_[comp_idx]->size;
    }

    void resize(size_t new_capacity) {
        assert(new_capacity > layout_.capacity);
        Layout new_layout = calculate_layout(types_, new_capacity);
        std::byte* new_data = static_cast<std::byte*>(alloc_->allocate(new_layout.total_size));

        // Relocate Entities
        std::memcpy(new_data + new_layout.entity_offset, data_ + layout_.entity_offset, count_ * sizeof(Entity));

        // Relocate Components
        for (size_t i = 0; i < types_.size(); ++i) {
            const auto* type = types_[i];
            std::byte* src_base = data_ + layout_.component_offsets[i];
            std::byte* dst_base = new_data + new_layout.component_offsets[i];

            if (type->hooks.move) {
                for (size_t k = 0; k < count_; ++k) {
                    void* src = src_base + k * type->size;
                    void* dst = dst_base + k * type->size;
                    type->hooks.move(dst, src);
                    if (type->hooks.dtor) type->hooks.dtor(src); 
                }
            } else {
                std::memcpy(dst_base, src_base, count_ * type->size);
            }
        }

        alloc_->deallocate(data_, layout_.total_size);
        data_ = new_data;
        layout_ = new_layout;
    }

    void destroy_at(size_t index) {
        for (size_t i = 0; i < types_.size(); ++i) {
            if (types_[i]->hooks.dtor) {
                types_[i]->hooks.dtor(component(i, index));
            }
        }
    }

    void clear() {
        for (size_t i = 0; i < count_; ++i) destroy_at(i);
        count_ = 0;
    }
    
    void move_from(Chunk* other, size_t src_index, size_t dst_index) {
        entity(dst_index) = other->entity(src_index);
        for (size_t i = 0; i < types_.size(); ++i) {
            void* src = other->component(i, src_index);
            void* dst = component(i, dst_index);
            const auto* type = types_[i];
            if (type->hooks.move) {
                type->hooks.move(dst, src);
                if (type->hooks.dtor) type->hooks.dtor(src); // 🌸 Move then Destruct
            }
            else std::memcpy(dst, src, type->size);
        }
    }

    size_t alloc_row() { return count_++; }
    void free_row() { count_--; }

private:
    std::byte* data_;
    size_t count_ = 0;
    Layout layout_;
    std::vector<const TypeInfo*> types_;
    Allocator* alloc_;
};

template<typename Config = DefaultConfig>
class Table {
public:
    static constexpr int  RawPageSize = Config::ChunkSize;
    static constexpr bool IsMonolithic = (RawPageSize == -1);
    static constexpr size_t InitialCapacity = IsMonolithic ? 1024 : static_cast<size_t>(RawPageSize);

    static_assert(IsMonolithic || std::has_single_bit(static_cast<size_t>(RawPageSize)), 
        "Elysia: PageSize must be power of 2 unless using Monolithic mode (-1)!");

    using ChunkType = Chunk;

    Table(std::span<const TypeInfo* const> sorted_types, Allocator* alloc)
        : alloc_(alloc) 
    {
        types_.assign(sorted_types.begin(), sorted_types.end());
    }

    [[nodiscard]] size_t count() const { return total_count_; }
    
    struct Location {
        ChunkType* chunk;
        size_t index;
    };

    [[nodiscard]] inline Location locate(size_t row) const noexcept {
        if constexpr (IsMonolithic) {
            // FIXED: Safety check for empty chunks in monolithic mode
            assert(!chunks_.empty() && "Table: Monolithic locate() called on empty chunk list!");
            return { chunks_[0].get(), row };
        } else {
            return { chunks_[row >> Shift].get(), row & Mask };
        }
    }

    void reserve(size_t minimum_capacity) {
        if constexpr (IsMonolithic) {
            // FIXED: Support initial allocation in reserve()
            if (chunks_.empty()) {
                add_chunk(std::max(InitialCapacity, minimum_capacity));
                return;
            }
            if (chunks_[0]->capacity() < minimum_capacity) {
                chunks_[0]->resize(minimum_capacity);
            }
        } else {
            size_t current_cap = chunks_.size() << Shift;
            if (minimum_capacity > current_cap) {
                size_t needed = minimum_capacity - current_cap;
                size_t chunks_needed = (needed + Mask) >> Shift;
                chunks_.reserve(chunks_.size() + chunks_needed);
                for(size_t i=0; i<chunks_needed; ++i) add_chunk(InitialCapacity);
            }
        }
    }

    [[nodiscard]] std::pair<ChunkType*, size_t> prepare_next_slot(size_t global_row) {
        if constexpr (IsMonolithic) {
            if (chunks_.empty()) [[unlikely]] {
                add_chunk(InitialCapacity);
            }
            auto& chunk = chunks_[0];
            if (chunk->is_full()) [[unlikely]] {
                size_t new_cap = chunk->capacity() * 2;
                chunk->resize(new_cap);
            }
            return { chunk.get(), chunk->alloc_row() };
        } else {
            size_t chunk_idx = global_row >> Shift;
            if (chunk_idx >= chunks_.size()) [[unlikely]] {
                add_chunk(InitialCapacity);
            }
            auto& chunk = chunks_[chunk_idx];
            return { chunk.get(), chunk->alloc_row() };
        }
    }

    /**
     * @brief Allocates a range of N rows at once.
     * Returns the global index of the first row in the batch.
     * Guaranteed to be contiguous within chunks in Monolithic mode.
     */
    size_t push_batch_raw(size_t count) {
        if (count == 0) return total_count_;
        size_t start_row = total_count_;
        
        if constexpr (IsMonolithic) {
            reserve(total_count_ + count);
            auto& chunk = chunks_[0];
            for (size_t i = 0; i < count; ++i) chunk->alloc_row();
        } else {
            // For Paged mode, we currently just call push_raw repeatedly
            // but in a tighter loop to avoid some overhead.
            for (size_t i = 0; i < count; ++i) {
                size_t global_row = total_count_++;
                auto m_ret = prepare_next_slot(global_row);
            }
            total_count_ = start_row + count;
            return start_row;
        }
        
        total_count_ += count;
        return start_row;
    }

    size_t push_raw(Entity e) {
        size_t global_row = total_count_++;
        auto [chunk, local_idx] = prepare_next_slot(global_row);
        chunk->entity(local_idx) = e;
        return global_row;
    }

    size_t push(Entity e) {
        size_t global_row = push_raw(e);
        auto loc = locate(global_row);
        for(size_t i=0; i<types_.size(); ++i) {
            if(types_[i]->hooks.ctor) {
                types_[i]->hooks.ctor(loc.chunk->component(i, loc.index));
            }
        }
        return global_row;
    }

    std::optional<Entity> swap_remove(size_t row) {
        if (total_count_ == 0) return std::nullopt;
        size_t last_row = total_count_ - 1;
        bool is_last = (row == last_row);
        auto loc_target = locate(row);
        
        Entity moved_entity;
        if (!is_last) {
            auto loc_last = locate(last_row);
            moved_entity = loc_last.chunk->entity(loc_last.index);
            
            // 1. 摧毁当前行的组件 (腾出坑位)
            loc_target.chunk->destroy_at(loc_target.index);
            
            // 2. 将最后一行搬过来 (move_from 内部会析构最后一行旧位)
            loc_target.chunk->move_from(loc_last.chunk, loc_last.index, loc_target.index);
            
            loc_last.chunk->free_row();
        } else {
            // 仅仅是最后一行，直接摧毁并收缩
            loc_target.chunk->destroy_at(loc_target.index);
            loc_target.chunk->free_row();
        }
        
        if constexpr (!IsMonolithic) {
            if (chunks_.back()->is_empty() && chunks_.size() > 1) chunks_.pop_back();
        }
        total_count_--;
        if (is_last) return std::nullopt;
        return moved_entity;
    }

    void clear() {
        for (auto& chunk : chunks_) chunk->clear();
        total_count_ = 0;
    }

    const std::vector<const TypeInfo*>& types() const { return types_; }
    const std::vector<std::unique_ptr<ChunkType>>& chunks() const { return chunks_; }

private:
    void add_chunk(size_t cap) {
        chunks_.push_back(std::make_unique<ChunkType>(cap, types_, alloc_));
    }

    static constexpr size_t Shift = IsMonolithic ? 0 : static_cast<size_t>(std::countr_zero(InitialCapacity));
    static constexpr size_t Mask  = IsMonolithic ? 0 : InitialCapacity - 1;

    std::vector<const TypeInfo*> types_;
    std::vector<std::unique_ptr<ChunkType>> chunks_;
    Allocator* alloc_;
    size_t total_count_ = 0;
};

} // namespace elysia
