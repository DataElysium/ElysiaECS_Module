module;
#include <vector>
#include <cstdint>
#include <optional>
#include <algorithm>
#include <span>
#include <atomic>
#include <cassert>

export module elysia.core;

import elysia.config;
import elysia.entity;
import elysia.result;

export namespace elysia {

struct ELYSIA_API EntityRecord {
    void* archetype = nullptr; 
    uint32_t row = 0; 
    uint16_t version = 0;
    bool active = false;
};

// --- Batch Result ---
struct ELYSIA_API EntityBatchResult {
    std::span<const uint32_t> recycled_ids;
    uint32_t new_start_id;
    uint32_t new_count;
};

class ELYSIA_API EntityIndex {
public:
    EntityIndex(size_t initial_capacity = 1024) {
        records_.resize(initial_capacity);
        for (size_t i = 0; i < initial_capacity; ++i) { 
            records_[i].active = false;
            records_[i].version = 0;
        }
        next_new_id_.store(0);
        recycled_head_.store(0);
    }

    EntityIndex(EntityIndex&& other) noexcept 
        : records_(std::move(other.records_)),
          recycled_ids_(std::move(other.recycled_ids_)) 
    {
        recycled_head_.store(other.recycled_head_.load());
        next_new_id_.store(other.next_new_id_.load());
    }

    EntityIndex& operator=(EntityIndex&& other) noexcept {
        if (this != &other) {
            records_ = std::move(other.records_);
            recycled_ids_ = std::move(other.recycled_ids_);
            recycled_head_.store(other.recycled_head_.load());
            next_new_id_.store(other.next_new_id_.load());
        }
        return *this;
    }

    // --- High Performance Atomic Allocation ---

    /**
     * @brief High-performance concurrent ID reservation.
     * @return A unique 32-bit ID.
     * @note ⚡ HOT PATH: This method is lock-free and thread-safe during the Parallel Phase.
     */
    [[nodiscard]] uint32_t reserve_id() {
        size_t head = recycled_head_.fetch_add(1, std::memory_order_relaxed);
        if (head < recycled_ids_.size()) {
            uint32_t id = recycled_ids_[head];
            assert(!records_[id].active && "Recycled pool contaminated by active entity!");
            return id;
        } else {
            return next_new_id_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Reserve multiple IDs at once, maximizing reuse of recycled pool.
     */
    [[nodiscard]] EntityBatchResult reserve_batch(uint32_t count) {
        size_t head = recycled_head_.fetch_add(count, std::memory_order_relaxed);
        
        uint32_t available_recycled = 0;
        const uint32_t* ptr = nullptr;
        
        if (head < recycled_ids_.size()) {
            available_recycled = static_cast<uint32_t>(std::min<size_t>(count, recycled_ids_.size() - head));
            ptr = &recycled_ids_[head];
        }
        
        uint32_t needed_new = count - available_recycled;
        uint32_t new_start = 0;
        if (needed_new > 0) {
            new_start = next_new_id_.fetch_add(needed_new, std::memory_order_relaxed);
        }
        
        return {
            std::span<const uint32_t>(ptr, available_recycled),
            new_start,
            needed_new
        };
    }

    /**
     * @brief Force reservation of mathematically continuous NEW IDs.
     */
    [[nodiscard]] uint32_t reserve_continuous(uint32_t count) {
        return next_new_id_.fetch_add(count, std::memory_order_relaxed);
    }

    // --- Lifecycle (Exclusive/Sync Point only) ---

    [[nodiscard]] Entity spawn() {
        uint32_t id = reserve_id();
        if (id >= records_.size()) { grow(id + 1); }
        EntityRecord& rec = records_[id];
        rec.active = true; 
        rec.archetype = nullptr; 
        rec.row = 0;
        return Entity(id, rec.version);
    }

    /**
     * @brief Attempt to spawn an entity at a specific handle.
     * @return Success handle or AlreadyExists error.
     */
    [[nodiscard]] Result<Entity> spawn_at(Entity e) {
        uint32_t id = e.id();
        if (id >= records_.size()) grow(id + 1);
        
        EntityRecord& rec = records_[id];
        if (rec.active) {
            if (rec.version == e.version()) return Result<Entity>::ok(e);
            return Result<Entity>::err(ErrorCode::AlreadyExists, "Entity ID already active");
        }

        // Truthful pool: remove from recycled_ids_ if present
        auto it = std::find(recycled_ids_.begin() + recycled_head_.load(), recycled_ids_.end(), id);
        if (it != recycled_ids_.end()) {
            recycled_ids_.erase(it);
        }

        rec.active = true;
        rec.archetype = nullptr;
        rec.row = 0;
        rec.version = e.version();
        
        // No contention in exclusive phase, simple store
        if (id >= next_new_id_.load()) {
            next_new_id_.store(id + 1, std::memory_order_relaxed);
        }

        return Result<Entity>::ok(e);
    }

    void free(Entity e) {
        uint32_t id = e.id();
        if (id >= records_.size()) return;
        EntityRecord& rec = records_[id];
        if (!rec.active || rec.version != e.version()) return;
        
        rec.active = false; 
        rec.version++;
        recycled_ids_.push_back(id);
    }

    void free_id(uint32_t id) {
        if (id >= records_.size()) return;
        EntityRecord& rec = records_[id];
        if (!rec.active) return;
        
        rec.active = false;
        rec.version++;
        recycled_ids_.push_back(id);
    }

    /**
     * @brief Consolidate recycled pool by removing consumed entries.
     */
    void cleanup_recycled_pool() {
        size_t head = recycled_head_.load();
        if (head == 0) return;

        if (head >= recycled_ids_.size()) {
            recycled_ids_.clear();
        } else {
            recycled_ids_.erase(recycled_ids_.begin(), recycled_ids_.begin() + head);
        }
        recycled_head_.store(0, std::memory_order_relaxed);
    }

    void grow(size_t new_capacity) {
        size_t old_cap = records_.size();
        records_.resize(new_capacity);
        for (size_t i = old_cap; i < new_capacity; ++i) {
            records_[i].active = false;
            records_[i].version = 0;
        }
        // Exclusive phase: simple store
        uint32_t cap = static_cast<uint32_t>(new_capacity);
        if (cap > next_new_id_.load()) {
            next_new_id_.store(cap, std::memory_order_relaxed);
        }
    }

    void reserve(size_t capacity) {
        if (capacity > records_.size()) grow(capacity);
    }

    void update(Entity e, void* arch, uint32_t row) {
        uint32_t id = e.id();
        if (id >= records_.size()) grow(id + 1);
        records_[id].archetype = arch;
        records_[id].row = row;
        records_[id].version = e.version();
        records_[id].active = true;
    }

    [[nodiscard]] Result<EntityRecord*> lookup(Entity e) {
        uint32_t id = e.id();
        if (id >= records_.size()) return Result<EntityRecord*>::err(ErrorCode::NotFound, "Out of range");
        EntityRecord& rec = records_[id];
        if (!rec.active || rec.version != e.version()) return Result<EntityRecord*>::err(ErrorCode::NotFound, "Mismatch");
        return Result<EntityRecord*>::ok(&rec);
    }

    [[nodiscard]] bool is_alive(Entity e) const {
        uint32_t id = e.id();
        if (id >= records_.size()) return false;
        const auto& rec = records_[id];
        return rec.active && rec.version == e.version();
    }

    std::vector<EntityRecord>& records() { return records_; }
    const std::vector<EntityRecord>& records() const { return records_; }

    // --- Serialization Support ---
    uint32_t next_new_id() const { return next_new_id_.load(); }
    void set_next_new_id(uint32_t val) { next_new_id_.store(val); }
    std::vector<uint32_t>& recycled_ids() { return recycled_ids_; }
    const std::vector<uint32_t>& recycled_ids() const { return recycled_ids_; }

private:
    std::vector<EntityRecord> records_;
    std::vector<uint32_t> recycled_ids_;
    
    // alignas to prevent False Sharing (typical L1 cache line is 64 bytes, 
    // but x86 prefetchers often pull 2 lines, so 128 is safer).
    alignas(128) std::atomic<size_t> recycled_head_{0};
    alignas(128) std::atomic<uint32_t> next_new_id_{0};
};

} // namespace elysia
