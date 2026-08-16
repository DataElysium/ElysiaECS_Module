module;
#include <vector>
#include <cstring>
#include <algorithm>
#include <span>
#include <memory>
#include <cassert>
#include <stdexcept>
#include <cstdio>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

module elysia.world;

namespace elysia {

World::World(Allocator* alloc) 
    : graph_(alloc) 
{
    Allocator* a = alloc ? alloc : get_default_allocator();
    buffer_a_ = std::make_shared<CommandBuffer>(&index_, std::shared_ptr<Allocator>(a, [](auto*){})); 
    buffer_b_ = std::make_shared<CommandBuffer>(&index_, std::shared_ptr<Allocator>(a, [](auto*){}));
}

 World::~World() {}

World::World(World&& other) noexcept 
    : index_(std::move(other.index_)),
      graph_(std::move(other.graph_)),
      observer_registry_(std::move(other.observer_registry_)),
      decorators_(std::move(other.decorators_)),
      resources_(std::move(other.resources_)),
      buffer_a_(std::move(other.buffer_a_)),
      buffer_b_(std::move(other.buffer_b_))
{
    if (buffer_a_) buffer_a_->set_index(&index_);
    if (buffer_b_) buffer_b_->set_index(&index_);
}

World& World::operator=(World&& other) noexcept {
    if (this != &other) {
        index_ = std::move(other.index_);
        graph_ = std::move(other.graph_);
        observer_registry_ = std::move(other.observer_registry_);
        decorators_ = std::move(other.decorators_);
        resources_ = std::move(other.resources_);
        buffer_a_ = std::move(other.buffer_a_);
        buffer_b_ = std::move(other.buffer_b_);
        if (buffer_a_) buffer_a_->set_index(&index_);
        if (buffer_b_) buffer_b_->set_index(&index_);
    }
    return *this;
}

// --- Entity Lifecycle ---

Result<Entity> World::spawn_at(Entity e) {
    auto index_res = index_.spawn_at(e);
    if (index_res.is_err()) return index_res;

    auto& rec = index_.records()[e.id()];
    if (rec.archetype) return Result<Entity>::ok(e);

    auto* root = graph_.root();
    auto modifier = root->modify();
    size_t row = modifier.push(e);
    
    index_.update(e, root, static_cast<uint32_t>(row));
    return Result<Entity>::ok(e);
}

EntityView World::spawn() {
    Entity e = index_.spawn();
    auto* root = graph_.root();
    auto modifier = root->modify();
    size_t row = modifier.push(e);
    index_.update(e, root, static_cast<uint32_t>(row));
    return {this, e};
}

void World::import_entities(uint32_t start, uint32_t count) {
    if (count == 0) return;

    std::vector<Entity> claimed;
    claimed.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entity e(start + i, 0);
        auto res = index_.spawn_at(e);
        if (res.is_ok()) claimed.push_back(e);
    }
    if (claimed.empty()) return;

    auto* root = graph_.root();
    size_t start_row = root->table().push_batch_raw(claimed.size());

    for (size_t i = 0; i < claimed.size(); ++i) {
        Entity e = claimed[i];
        auto loc = root->table().locate(start_row + i);
        loc.chunk->entity(loc.index) = e;
        index_.update(e, root, static_cast<uint32_t>(start_row + i));
    }
}

void World::despawn(Entity e) {
    auto res = index_.lookup(e);
    if (res.is_err()) return;
    EntityRecord* rec = res.unwrap();
    auto* arch = static_cast<Archetype<DefaultConfig>*>(rec->archetype);
    
    // 🌸 Notify Observers BEFORE removal (so data is accessible)
    if (arch) {
        for (const auto* type_info : arch->types()) {
            bool has = observer_registry_.has_observer(ObserverEvent::OnRemove, type_info->id);
            if (has) {
                observer_registry_.notify(ObserverEvent::OnRemove, type_info->id, e);
            }
        }
    }

    auto modifier = arch->modify();
    auto moved_entity = modifier.swap_remove(rec->row);
    if (moved_entity) index_.update(*moved_entity, arch, rec->row);
    index_.free(e);
}

void World::despawn(uint32_t id) {
    if (id >= index_.records().size()) return;
    auto& rec = index_.records()[id];
    if (!rec.active) return;
    despawn(Entity(id, rec.version));
}

size_t World::drop_archetypes_with(uint64_t type_id) {
    using ArchType = Archetype<DefaultConfig>;

    size_t removed = 0;
    std::vector<Entity> observer_path;

    graph_.each([&](ArchType* arch) {
        if (!arch || arch->count() == 0) return;
        if (!arch->get_column_index(type_id)) return;

        bool requires_observer_path = false;
        for (const auto* type_info : arch->types()) {
            if (observer_registry_.has_observer(ObserverEvent::OnRemove, type_info->id)) {
                requires_observer_path = true;
                break;
            }
        }

        if (requires_observer_path) {
            observer_path.clear();
            observer_path.reserve(arch->count());
            for (size_t row = 0; row < arch->count(); ++row) {
                auto loc = arch->table().locate(row);
                observer_path.push_back(loc.chunk->entity(loc.index));
            }
            for (Entity e : observer_path) {
                if (!index_.is_alive(e)) continue;
                despawn(e);
                removed++;
            }
            return;
        }

        const size_t count = arch->count();
        for (size_t row = 0; row < count; ++row) {
            auto loc = arch->table().locate(row);
            index_.free(loc.chunk->entity(loc.index));
        }
        arch->modify().clear();
        removed += count;
    });

    return removed;
}

void World::batch_despawn(std::span<const Entity> entities) {
    if (entities.empty()) return;

    using ArchType = Archetype<DefaultConfig>;
    struct Pending {
        Entity entity;
        ArchType* arch;
        uint32_t row;
    };

    std::vector<Entity> unique_entities;
    unique_entities.reserve(entities.size());
    std::unordered_set<Entity> seen;
    seen.reserve(entities.size());

    bool requires_observer_path = false;
    for (Entity e : entities) {
        if (!seen.insert(e).second) continue;
        auto res = index_.lookup(e);
        if (res.is_err()) continue;

        auto* arch = static_cast<ArchType*>(res.unwrap()->archetype);
        if (!arch) continue;

        unique_entities.push_back(e);
        for (const auto* type_info : arch->types()) {
            if (observer_registry_.has_observer(ObserverEvent::OnRemove, type_info->id)) {
                requires_observer_path = true;
                break;
            }
        }
    }

    if (requires_observer_path) {
        for (Entity e : unique_entities) despawn(e);
        return;
    }

    std::unordered_map<ArchType*, std::vector<Pending>> groups;
    groups.reserve(unique_entities.size());
    for (Entity e : unique_entities) {
        auto res = index_.lookup(e);
        if (res.is_err()) continue;
        EntityRecord* rec = res.unwrap();
        auto* arch = static_cast<ArchType*>(rec->archetype);
        if (!arch) continue;
        groups[arch].push_back(Pending{e, arch, rec->row});
    }

    for (auto& [arch, rows] : groups) {
        std::sort(rows.begin(), rows.end(), [](const Pending& a, const Pending& b) {
            return a.row > b.row;
        });

        auto modifier = arch->modify();
        for (const auto& item : rows) {
            auto res = index_.lookup(item.entity);
            if (res.is_err()) continue;
            EntityRecord* rec = res.unwrap();
            if (rec->archetype != arch || rec->row != item.row) {
                despawn(item.entity);
                continue;
            }

            auto moved_entity = modifier.swap_remove(item.row);
            if (moved_entity) index_.update(*moved_entity, arch, item.row);
            index_.free(item.entity);
        }
    }
}

// --- Component Management (Dynamic) ---

void World::add_component_dynamic(Entity e, const TypeInfo* type_info, void* data) {
    if (!type_info) return;
    graph_.registry().ensure_registered(type_info);

    auto res_lookup = index_.lookup(e);
    if (res_lookup.is_err()) return;
    EntityRecord* rec = res_lookup.unwrap();
    
    using ArchType = Archetype<DefaultConfig>;
    if (!rec->archetype) rec->archetype = graph_.root();
    ArchType* current_arch = static_cast<ArchType*>(rec->archetype);

    auto* next_arch = graph_.traverse_add(current_arch, type_info->id);
    if (!next_arch) return;

    if (next_arch == current_arch) {
        auto col = next_arch->get_column_index(type_info->id);
        if(col) {
            auto loc = next_arch->table().locate(rec->row);
            void* ptr = loc.chunk->component(*col, loc.index);
            if(type_info->hooks.dtor) type_info->hooks.dtor(ptr);
            if(type_info->hooks.move) type_info->hooks.move(ptr, data);
            else std::memcpy(ptr, data, type_info->size);
            
            // Notify OnAdd
            observer_registry_.notify(ObserverEvent::OnAdd, type_info->id, e);
        }
    } else {
        auto src_mod = current_arch->modify();
        auto dst_mod = next_arch->modify();
        size_t new_row = dst_mod.push_raw(e); 
        auto src_loc = current_arch->table().locate(rec->row);
        auto dst_loc = next_arch->table().locate(new_row);

        auto src_types = current_arch->types();
        auto dst_types = next_arch->types();
        size_t i = 0, j = 0;
        while (i < src_types.size() && j < dst_types.size()) {
            if (src_types[i]->id < dst_types[j]->id) {
                 i++;
            }
            else if (dst_types[j]->id < src_types[i]->id) {
                 j++;
            }
            else {
                void* src_ptr = src_loc.chunk->component(i, src_loc.index);
                void* dst_ptr = dst_loc.chunk->component(j, dst_loc.index);
                
                const auto& hooks = dst_types[j]->hooks;
                if (hooks.move) hooks.move(dst_ptr, src_ptr);
                else std::memcpy(dst_ptr, src_ptr, dst_types[j]->size);
                i++; j++;
            }
        }

        auto new_col = next_arch->get_column_index(type_info->id);
        if(new_col) {
            void* ptr = dst_loc.chunk->component(*new_col, dst_loc.index);
            const auto& hooks = type_info->hooks;
            if(hooks.move) hooks.move(ptr, data);
            else std::memcpy(ptr, data, type_info->size);
        }

        auto moved_entity = src_mod.swap_remove(rec->row);
        if (moved_entity) index_.update(*moved_entity, current_arch, rec->row);
        index_.update(e, next_arch, static_cast<uint32_t>(new_row));
        
        // Notify OnAdd
        observer_registry_.notify(ObserverEvent::OnAdd, type_info->id, e);
    }
}

void World::remove_component_dynamic(Entity e, uint64_t type_id) {
    auto res_lookup = index_.lookup(e);
    if (res_lookup.is_err()) return;
    EntityRecord* rec = res_lookup.unwrap();
    if (!rec->archetype) return;

    using ArchType = Archetype<DefaultConfig>;
    ArchType* current_arch = static_cast<ArchType*>(rec->archetype);

    auto local_idx = graph_.registry().local_index(type_id);
    if (local_idx == MetaConfig::INVALID_LOCAL_ID || !current_arch->bit_sig().test(local_idx)) return;
    
    // Notify OnRemove
    if (observer_registry_.has_observer(ObserverEvent::OnRemove, type_id)) {
        observer_registry_.notify(ObserverEvent::OnRemove, type_id, e);
    }
    
    auto* next_arch = graph_.traverse_remove(current_arch, type_id);
    if (!next_arch || next_arch == current_arch) return;

    auto src_mod = current_arch->modify();
    auto dst_mod = next_arch->modify();
    size_t new_row = dst_mod.push_raw(e);
    auto src_loc = current_arch->table().locate(rec->row);
    auto dst_loc = next_arch->table().locate(new_row);

    auto src_types = current_arch->types();
    auto dst_types = next_arch->types();
    size_t i = 0, j = 0;
    while (i < src_types.size() && j < dst_types.size()) {
        if (src_types[i]->id < dst_types[j]->id) i++;
        else if (dst_types[j]->id < src_types[i]->id) j++;
        else {
            void* src_ptr = src_loc.chunk->component(i, src_loc.index);
            void* dst_ptr = dst_loc.chunk->component(j, dst_loc.index);
            const auto& hooks = dst_types[j]->hooks;
            if (hooks.move) hooks.move(dst_ptr, src_ptr);
            else std::memcpy(dst_ptr, src_ptr, dst_types[j]->size);
            i++; j++;
        }
    }

    auto moved_entity = src_mod.swap_remove(rec->row);
    if (moved_entity) index_.update(*moved_entity, current_arch, rec->row);
    index_.update(e, next_arch, static_cast<uint32_t>(new_row));
}

// --- WorldBatchAccessor Implementation ---

void WorldBatchAccessor::spawn_bundle(Entity e, std::span<const TypeInfo* const> types, std::span<void* const> datas) {
    auto* arch = world->graph().get_or_create(types);
    if (!arch) return;

    // 🌸 FIX: If entity already has an archetype (e.g. from world.spawn()), 
    // we must remove it from there first to avoid duplication!
    auto res_lookup = world->index().lookup(e);
    if (res_lookup.is_ok()) {
        EntityRecord* rec = res_lookup.unwrap();
        if (rec->archetype) {
            auto* old_arch = static_cast<Archetype<DefaultConfig>*>(rec->archetype);
            if (old_arch != arch) {
                auto modifier = old_arch->modify();
                auto moved_entity = modifier.swap_remove(rec->row);
                if (moved_entity) world->index().update(*moved_entity, old_arch, rec->row);
                rec->archetype = nullptr; // Clear before re-assigning
            } else {
                // Already in the correct archetype? Just update data.
                // For a "spawn" bundle, we usually assume it's a new insertion, 
                // but let's be safe.
            }
        }
    }

    auto modifier = arch->modify();
    size_t row = modifier.push_raw(e);
    auto loc = arch->table().locate(row);
    auto arch_types = arch->types();

    for(size_t i=0; i<arch_types.size(); ++i) {
        void* ptr = loc.chunk->component(i, loc.index);
        const auto* type = arch_types[i];
        bool initialized = false;
        for (size_t k=0; k<types.size(); ++k) {
            if (types[k]->id == type->id) {
                if(type->hooks.move) type->hooks.move(ptr, datas[k]);
                else std::memcpy(ptr, datas[k], type->size);
                initialized = true;
                break;
            }
        }
        if (!initialized && type->hooks.ctor) type->hooks.ctor(ptr);
    }
    world->index().update(e, arch, static_cast<uint32_t>(row));

    // 🌸 Notify OnAdd for all components in the bundle
    for (const auto* type : types) {
        world->observer_registry_.notify(ObserverEvent::OnAdd, type->id, e);
    }
}

// --- CommandAccessor Entry ---

void CommandAccessor::submit(CommandBuffer& cmd) { world->submit(cmd); }

// --- Resource Access ---

void* World::get_resource_dynamic(uint64_t id) const {
    auto it = resources_.find(id);
    return (it != resources_.end()) ? it->second.ptr : nullptr;
}

} // namespace elysia

