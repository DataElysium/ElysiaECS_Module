module;
#include <memory_resource>
#include <memory>
#include <cstddef>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <cassert>

export module elysia.mem;

export namespace elysia {

namespace pmr = std::pmr;
using Allocator = pmr::memory_resource;

inline Allocator* get_default_allocator() {
    return pmr::new_delete_resource();
}

/**
 * @brief AuditMemoryResource: A non-intrusive runtime memory auditor.
 * Tracks allocations, size mismatch, and leaks without compiler-level instrumenting.
 */
class AuditResource final : public Allocator {
public:
    explicit AuditResource(Allocator* upstream = get_default_allocator(), std::string_view name = "Audit")
        : upstream_(upstream), name_(name) {}

    ~AuditResource() override {
        report_leaks();
    }

    void report_leaks() {
        std::lock_guard lock(mutex_);
        if (allocations_.empty()) return;

        std::cerr << "\n--- [Elysia Audit] Memory Leaks Detected in '" << name_ << "' ---\"n";
        size_t total = 0;
        for (const auto& [ptr, size] : allocations_) {
            std::cerr << "  Leak: " << ptr << " (" << size << " bytes)\n";
            total += size;
        }
        std::cerr << "Total leaked: " << total << " bytes in " << allocations_.size() << " blocks.\n";
        std::cerr << "---------------------------------------------------\"n" << std::endl;
        assert(allocations_.empty() && "Memory leaks identified by AuditResource!");
    }

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* p = upstream_->allocate(bytes, alignment);
        {
            std::lock_guard lock(mutex_);
            allocations_[p] = bytes;
        }
        return p;
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        {
            std::lock_guard lock(mutex_);
            auto it = allocations_.find(p);
            if (it == allocations_.end()) {
                std::cerr << "[Elysia Audit] CRITICAL: Attempted to deallocate untracked pointer: " << p << std::endl;
                assert(false && "Bad deallocate: untracked pointer");
            }
            if (it->second != bytes) {
                std::cerr << "[Elysia Audit] CRITICAL: Size mismatch on deallocate for " << p 
                          << ". Allocated: " << it->second << ", Deallocating: " << bytes << std::endl;
                assert(false && "Bad deallocate: size mismatch");
            }
            allocations_.erase(it);
        }
        upstream_->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    Allocator* upstream_;
    std::string name_;
    std::mutex mutex_;
    // We use a standard map here; in super-strict mode we'd use a fixed-pool map
    std::unordered_map<void*, size_t> allocations_;
};

// ... (PmrDeleter and make_unique_pmr remain same)

template<typename T>
struct PmrDeleter {
    Allocator* alloc;
    PmrDeleter(Allocator* a = get_default_allocator()) : alloc(a) {}
    void operator()(T* ptr) const {
        if (ptr) {
            ptr->~T();
            alloc->deallocate(ptr, sizeof(T), alignof(T));
        }
    }
};

template<typename T>
using UniquePtr = std::unique_ptr<T, PmrDeleter<T>>;

template<typename T, typename... Args>
UniquePtr<T> make_unique_pmr(Allocator* alloc, Args&&... args) {
    void* mem = alloc->allocate(sizeof(T), alignof(T));
    try {
        T* ptr = new (mem) T(std::forward<Args>(args)...);
        return UniquePtr<T>(ptr, PmrDeleter<T>(alloc));
    } catch (...) {
        alloc->deallocate(mem, sizeof(T), alignof(T));
        throw;
    }
}

} // namespace elysia