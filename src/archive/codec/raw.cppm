module;
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <type_traits>
#include <cstdint>
#include <span>

export module elysia.archive:codec.raw;

import :model;
import :stream;
import :registry;
import elysia.world;
import elysia.entity;
import elysia.storage;
import elysia.graph;
import elysia.config;
import elysia.result;
import elysia.core;
import elysia.meta;
import elysia.table;

export namespace elysia::archive {

/**
 * @brief Zero-Overhead Memory Snapshot Header (Wild Mode v3).
 */
struct RawWorldHeader {
    char magic[8] = {'E','L','Y','S','I','A','\0','R'};
    uint32_t version = 3; // Bumping to 3 for new EntityIndex
    uint32_t endian_check = 0x12345678;
    uint32_t ptr_size = sizeof(void*);
    uint32_t archetype_count = 0;
};

struct RawSkeletonHeader {
    uint32_t next_new_id;
    uint32_t record_capacity;
    uint32_t recycled_count;
    uint32_t segment_count;
    uint32_t nonzero_version_count;
};

struct RawVersionPatch {
    uint32_t id;
    uint16_t version;
};

struct RawArchetypeHeader {
    uint32_t entity_count = 0;
    uint32_t component_count = 0;
};

struct RawColumnHeader {
    uint64_t type_id = 0;
    uint32_t size = 0;
    uint32_t alignment = 0; 
    char name[64] = {0}; 
};

// --- World Packing (Wild Mode) ---

template<OutputStream S>
Result<void> pack_world_raw(S& stream, World& world, bool unchecked = false) {
    // 1. SAFETY SCAN: Ensure all components are POD-safe
    if (!unchecked) {
        for (size_t i = 0; i < world.archetype_count(); ++i) {
            auto* arch = world.graph().get_archetype(i);
            if (!arch || arch->count() == 0) continue;
            for (const auto* info : arch->types()) {
                if (info->hooks.dtor || info->hooks.move) {
                    return Result<void>::err(ErrorCode::InvalidOperation, 
                        "Unsafe Raw Pack: Component '" + std::string(info->name) + 
                        "' has custom lifecycle hooks (non-POD).");
                }
            }
        }
    }

    RawWorldHeader h;
    h.archetype_count = static_cast<uint32_t>(world.archetype_count());
    if (auto r = stream.write_bytes(&h, sizeof(h)); r.is_err()) return r;

    // 2. Prepare Sparse Skeleton Data
    const auto& records = world.index().records();
    std::vector<EntitySegment> segments;
    std::vector<RawVersionPatch> patches;

    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].active) {
            size_t start = i;
            while (i < records.size() && records[i].active) {
                if (records[i].version > 0) {
                    patches.push_back({static_cast<uint32_t>(i), records[i].version});
                }
                i++;
            }
            segments.push_back({static_cast<uint64_t>(start), static_cast<uint32_t>(i - start)});
            i--; 
        }
    }

    const auto& recycled = world.index().recycled_ids();

    RawSkeletonHeader sh;
    sh.next_new_id = world.index().next_new_id();
    sh.record_capacity = static_cast<uint32_t>(records.size());
    sh.recycled_count = static_cast<uint32_t>(recycled.size());
    sh.segment_count = static_cast<uint32_t>(segments.size());
    sh.nonzero_version_count = static_cast<uint32_t>(patches.size());

    if (auto r = stream.write_bytes(&sh, sizeof(sh)); r.is_err()) return r;
    
    if (!recycled.empty()) {
        if (auto r = stream.write_bytes(recycled.data(), recycled.size() * sizeof(uint32_t)); r.is_err()) return r;
    }
    
    if (!segments.empty()) {
        if (auto r = stream.write_bytes(segments.data(), segments.size() * sizeof(EntitySegment)); r.is_err()) return r;
    }
    if (!patches.empty()) {
        if (auto r = stream.write_bytes(patches.data(), patches.size() * sizeof(RawVersionPatch)); r.is_err()) return r;
    }

    // 3. Write Archetypes (The Meat)
    for (size_t i = 0; i < world.archetype_count(); ++i) {
        auto* arch = world.graph().get_archetype(i);
        if (!arch) continue;

        RawArchetypeHeader ah;
        ah.entity_count = arch->count();
        auto types = arch->types();
        ah.component_count = static_cast<uint32_t>(types.size());

        if (auto r = stream.write_bytes(&ah, sizeof(ah)); r.is_err()) return r;

        for (const auto* info : types) {
            RawColumnHeader ch;
            ch.type_id = info->id;
            ch.size = static_cast<uint32_t>(info->size);
            ch.alignment = static_cast<uint32_t>(info->alignment);
            std::memcpy(ch.name, info->name.data(), std::min(info->name.size(), sizeof(ch.name) - 1));
            if (auto r = stream.write_bytes(&ch, sizeof(ch)); r.is_err()) return r;
        }

        if (ah.entity_count == 0) continue;

        for (auto& ck : arch->table().chunks()) {
            if (auto r = stream.write_bytes(&ck->entity(0), ck->count() * sizeof(Entity)); r.is_err()) return r;
        }

        for (size_t c = 0; c < ah.component_count; ++c) {
            size_t col_size = types[c]->size;
            for (auto& ck : arch->table().chunks()) {
                void* data = ck->component(c, 0);
                if (auto r = stream.write_bytes(data, ck->count() * col_size); r.is_err()) return r;
            }
        }
    }
    return Result<void>::ok();
}

// --- Internal Helpers ---

template<InputStream S>
Result<void> stream_into_chunks(S& stream, Archetype<>& storage, size_t start_row, size_t total_count, size_t elem_size, auto writer_fn) {
    size_t processed = 0;
    size_t current_row = start_row;
    while(processed < total_count) {
        auto loc = storage.table().locate(current_row);
        size_t batch = std::min(loc.chunk->capacity() - loc.index, total_count - processed);
        void* dest = writer_fn(loc.chunk, loc.index);
        if (auto r = stream.read_bytes(dest, batch * elem_size); r.is_err()) return r;
        processed += batch;
        current_row += batch;
    }
    return Result<void>::ok();
}

// --- World Unpacking (Wild Mode) ---

/**
 * @brief Unpack a raw world.
 */
template<InputStream S>
Result<void> unpack_world_raw(S& stream, World& world) {
    RawWorldHeader h;
    if (auto r = stream.read_bytes(&h, sizeof(h)); r.is_err()) return r;

    if (std::strncmp(h.magic, "ELYSIA", 6) != 0) return Result<void>::err(ErrorCode::InternalError, "Invalid Magic");
    if (h.version != 2 && h.version != 3) return Result<void>::err(ErrorCode::InvalidOperation, "Raw format version mismatch");

    // 1. Restore EntityIndex Skeleton
    RawSkeletonHeader sh;
    
    auto& records = world.index().records();
    
    if (h.version == 2) {
        // Legacy Version 2
        struct RawSkeletonHeaderV2 {
            uint32_t next_free_id;
            uint32_t record_capacity;
            uint32_t segment_count;
            uint32_t nonzero_version_count;
        } sh2;
        if (auto r = stream.read_bytes(&sh2, sizeof(sh2)); r.is_err()) return r;
        sh.next_new_id = sh2.record_capacity; // Rough estimate
        sh.record_capacity = sh2.record_capacity;
        sh.recycled_count = 0;
        sh.segment_count = sh2.segment_count;
        sh.nonzero_version_count = sh2.nonzero_version_count;
    } else {
        if (auto r = stream.read_bytes(&sh, sizeof(sh)); r.is_err()) return r;
    }
    
    records.assign(sh.record_capacity, EntityRecord{}); 
    world.index().set_next_new_id(sh.next_new_id);

    if (sh.recycled_count > 0) {
        world.index().recycled_ids().resize(sh.recycled_count);
        if (auto r = stream.read_bytes(world.index().recycled_ids().data(), sh.recycled_count * sizeof(uint32_t)); r.is_err()) return r;
    }

    if (sh.segment_count > 0) {
        std::vector<EntitySegment> segments(sh.segment_count);
        if (auto r = stream.read_bytes(segments.data(), sh.segment_count * sizeof(EntitySegment)); r.is_err()) return r;
        
        for (const auto& seg : segments) {
            for (uint32_t i = 0; i < seg.count; ++i) {
                size_t id = static_cast<size_t>(seg.start + i);
                if (id < records.size()) records[id].active = true;
            }
        }
    }

    // 🌸 Patch Versions
    if (sh.nonzero_version_count > 0) {
        std::vector<RawVersionPatch> patches(sh.nonzero_version_count);
        if (auto r = stream.read_bytes(patches.data(), sh.nonzero_version_count * sizeof(RawVersionPatch)); r.is_err()) return r;
        
        for (const auto& patch : patches) {
            if (patch.id < records.size()) records[patch.id].version = patch.version;
        }
    }

    // 2. Restore Archetypes
    for (uint32_t i = 0; i < h.archetype_count; ++i) {
        RawArchetypeHeader ah;
        if (auto r = stream.read_bytes(&ah, sizeof(ah)); r.is_err()) return r;

        std::vector<RawColumnHeader> cols(ah.component_count);
        std::vector<const TypeInfo*> target_types;
        for (uint32_t c = 0; c < ah.component_count; ++c) {
            if (auto r = stream.read_bytes(&cols[c], sizeof(RawColumnHeader)); r.is_err()) return r;
            const auto* info = world.graph().registry().get_info(cols[c].type_id);
            if (!info) {
                 info = world.graph().registry().register_opaque(cols[c].type_id, cols[c].name, cols[c].size, cols[c].alignment);
            }
            target_types.push_back(info);
        }

        auto* arch = world.graph().get_or_create(target_types);
        using ArchType = Archetype<DefaultConfig>;
        auto* storage = static_cast<ArchType*>(arch);

        if (ah.entity_count == 0) continue;
        size_t start_row = storage->modify().push_batch_raw(ah.entity_count);

        Result<void> res = stream_into_chunks(stream, *storage, start_row, ah.entity_count, sizeof(Entity), [](Chunk* c, size_t idx) {
            return &c->entity(idx);
        });
        if (res.is_err()) return res;

        for (uint32_t k = 0; k < ah.entity_count; ++k) {
            auto loc = storage->table().locate(start_row + k);
            Entity e = loc.chunk->entity(loc.index);
            if (e.id() < records.size() && records[e.id()].active && records[e.id()].version == e.version()) {
                records[e.id()].archetype = storage;
                records[e.id()].row = static_cast<uint32_t>(start_row + k);
            }
        }

        for (uint32_t c = 0; c < ah.component_count; ++c) {
            auto col_idx = *storage->get_column_index(cols[c].type_id);
            res = stream_into_chunks(stream, *storage, start_row, ah.entity_count, cols[c].size, [&](Chunk* chunk, size_t idx) {
                 return (char*)chunk->component(col_idx, 0) + (idx * cols[c].size);
            });
            if (res.is_err()) return res;
        }
    }
    return Result<void>::ok();
}

// --- RawTableCodec (Per-Archetype Path B) ---

class RawTableCodec {
public:
    /**
     * @brief Exports a single archetype to a raw binary stream.
     * No headers, just raw entity IDs and component columns.
     */
    static std::vector<char> export_archetype(Archetype<>& arch, const std::vector<const ComponentFactory*>& active) {
        std::vector<char> buf;
        VectorWriter writer{buf};

        // 1. Entities
        for (auto& ck : arch.table().chunks()) {
            writer.write_bytes(&ck->entity(0), ck->count() * sizeof(Entity));
        }

        // 2. Components
        for (auto* fac : active) {
            auto col_idx = arch.get_column_index(fac->type_id);
            if (!col_idx) continue;
            for (auto& ck : arch.table().chunks()) {
                writer.write_bytes(ck->component(*col_idx, 0), ck->count() * fac->size);
            }
        }
        return buf;
    }

    /**
     * @brief Loads a single archetype from raw bytes.
     */
    /**
     * @brief Loads a single archetype from raw bytes.
     * Uses CommandBuffer to support "Patching" (merging with existing entities).
     */
    static Result<void> load(World& world, CommandBuffer& cmd, 
                            const std::vector<const ComponentFactory*>& active,
                            std::span<const char> bytes, 
                            uint32_t entity_count,
                            const std::unordered_map<uint64_t, Entity>& id_map) {
        SpanReader reader{bytes};
        
        // 1. Entities
        std::vector<Entity> entities(entity_count);
        if (auto r = reader.read_bytes(entities.data(), entity_count * sizeof(Entity)); r.is_err()) return r;

        // 2. Components
        for (auto* fac : active) {
            // Read the whole column for this component
            std::vector<char> col_data(entity_count * fac->size);
            if (auto r = reader.read_bytes(col_data.data(), col_data.size()); r.is_err()) return r;

            for (uint32_t i = 0; i < entity_count; ++i) {
                if (!id_map.contains(entities[i].id())) continue;
                Entity e = id_map.at(entities[i].id());
                
                // Use CommandBuffer to insert the component data.
                // This is the "Patching" magic: CommandBuffer will merge multiple inserts.
                void* src_ptr = col_data.data() + (i * fac->size);
                
                // We need to add a dynamic insert method to CommandBuffer or use a workaround.
                // Since Registry has the generic->from_generic_cmd, we can use that if available,
                // but Raw path shouldn't use Generic. 
                // Let's assume Registry provides a direct blob insert for CommandBuffer.
                if (fac->generic) {
                    auto g = fac->generic->to_generic(src_ptr);
                    auto res = fac->generic->from_generic_cmd(cmd, e, g);
                    if (res.is_err()) return res;
                }
            }
        }

        return Result<void>::ok();
    }
};

} // namespace elysia::archive
