module;
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <span>
#include <iostream>
#include <cassert>

export module elysia.graph;

import elysia.config;
import elysia.meta;
import elysia.storage;
import elysia.mem;
import elysia.bitset;
import elysia.result;

export namespace elysia {

/**
 * @brief Zero-Allocation Archetype Registry with Local Component Dictionary.
 */
template<typename Config = DefaultConfig>
class ArchetypeGraph {
public:
    using ArchetypeNode = Archetype<Config>;
    using id_type = uint32_t;
    using SigKey = SignatureBuffer<>;

    explicit ArchetypeGraph(Allocator* alloc = nullptr)
        : alloc_(alloc ? alloc : get_default_allocator()) 
    {
        get_or_create(std::span<const uint64_t>{});
    }

    ArchetypeNode* root() const { return archetype_pool_[0].get(); }

    ArchetypeNode* traverse_add(ArchetypeNode* from, uint64_t component_id) {
        uint32_t local_idx = registry_.local_index(component_id);
        
        // 🌸 Fix: If not registered, we must have at least the TypeInfo 
        // to register it. Since we only have the ID here, we assume it 
        // was pre-registered in World::add_component_dynamic.
        // If still not found, we cannot proceed.
        if (local_idx == MetaConfig::INVALID_LOCAL_ID) {
            const auto* info = registry_.get_info(component_id);
            if (!info) return from; // Still not found
            local_idx = registry_.ensure_registered(info);
        }

        auto next_sig = from->bit_sig();
        if (next_sig.test(local_idx)) return from;
        next_sig.set(local_idx);

        auto it = sig_to_id_.find(next_sig);
        if (it != sig_to_id_.end()) return archetype_pool_[it->second].get();

        auto from_types = from->types();
        std::vector<const TypeInfo*> next_types;
        next_types.reserve(from_types.size() + 1);
        for(auto* t : from_types) next_types.push_back(t);
        
        const auto* info = registry_.get_info(component_id);
        if(info) next_types.push_back(info);
        
        return materialize(next_types, next_sig);
    }

    // NEW: Re-add TypeInfo overload for convenience and to fix tests
    ArchetypeNode* traverse_add(ArchetypeNode* from, const TypeInfo& type) {
        registry_.ensure_registered(&type);
        return traverse_add(from, type.id);
    }

    ArchetypeNode* traverse_remove(ArchetypeNode* from, uint64_t component_id) {
        uint32_t local_idx = registry_.local_index(component_id);
        if (local_idx == MetaConfig::INVALID_LOCAL_ID) return from;

        auto next_sig = from->bit_sig();
        if (!next_sig.test(local_idx)) return from;

        next_sig.reset(local_idx);
        auto it = sig_to_id_.find(next_sig);
        if (it != sig_to_id_.end()) return archetype_pool_[it->second].get();

        auto from_types = from->types();
        std::vector<const TypeInfo*> next_types;
        next_types.reserve(from_types.size());
        for(auto* t : from_types) if(t->id != component_id) next_types.push_back(t);

        return materialize(next_types, next_sig);
    }

    ArchetypeNode* get_or_create(std::span<const TypeInfo* const> types) {
        SigKey search_sig;
        for (const auto* t : types) search_sig.set(registry_.ensure_registered(t));
        
        auto it = sig_to_id_.find(search_sig);
        if (it != sig_to_id_.end()) return archetype_pool_[it->second].get();
        
        return materialize(types, search_sig);
    }

    ArchetypeNode* get_or_create(std::span<const uint64_t> ids) {
        SigKey search_sig;
        for (uint64_t id : ids) {
            auto local = registry_.local_index(id);
            if (local != MetaConfig::INVALID_LOCAL_ID) search_sig.set(local);
        }
        
        auto it = sig_to_id_.find(search_sig);
        if (it != sig_to_id_.end()) return archetype_pool_[it->second].get();

        std::vector<const TypeInfo*> infos;
        infos.reserve(ids.size());
        for (uint64_t id : ids) {
            const auto* info = registry_.get_info(id);
            if (info) infos.push_back(info);
        }
        return materialize(infos, search_sig);
    }

    ArchetypeNode* get_archetype(size_t index) const {
        assert(index < archetype_pool_.size() && "ArchetypeGraph::get_archetype index out of bounds!");
        return archetype_pool_[index].get();
    }

    void each(std::function<void(ArchetypeNode*)> func) {
        for (auto& node : archetype_pool_) func(node.get());
    }

    size_t archetype_count() const {
        return archetype_pool_.size();
    }

    Allocator* allocator() const { return alloc_; }
    ComponentRegistry& registry()   { return registry_; }
    const ComponentRegistry& registry() const { return registry_; }

private:
    ArchetypeNode* materialize(std::span<const TypeInfo* const> types, const SigKey& sig) {
        std::vector<const TypeInfo*> sorted_types(types.begin(), types.end());
        std::sort(sorted_types.begin(), sorted_types.end(), [](const TypeInfo* a, const TypeInfo* b) {
            return a->id < b->id;
        });

        id_type new_id = static_cast<id_type>(archetype_pool_.size());
        auto node = std::make_unique<ArchetypeNode>(sorted_types, alloc_, new_id, &registry_);
        ArchetypeNode* ptr = node.get();
        
        sig_to_id_[sig] = new_id;
        archetype_pool_.push_back(std::move(node));
        return ptr;
    }

    Allocator* alloc_;
    ComponentRegistry registry_; 
    std::unordered_map<SigKey, id_type> sig_to_id_;
    std::vector<std::unique_ptr<ArchetypeNode>> archetype_pool_;
};

} // namespace elysia