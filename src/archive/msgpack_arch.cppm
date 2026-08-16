module;
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstdint>

export module elysia.archive:msgpack_arch;

import :model;
import :registry;
import :utils;
import :codec.columnar; 
import elysia.reflect_wrapper;
import elysia.result;
import elysia.world;
import elysia.entity;
import elysia.storage;

export namespace elysia::archive {

class ELYSIA_ARCHIVE_API MsgPackArchive {
public:
    static std::vector<char> create(World& world, const SnapshotRegistry& reg);
    static Result<void> load(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, const std::vector<char>& data);
    
    static Result<void> load_with_remap(World& world, CommandBuffer& cmd, 
                                      const SnapshotRegistry& reg, 
                                      const IDRemapRegistry& id_reg,
                                      const IDMapper& mapper,
                                      const std::vector<char>& data);
};

// Internal Data Struct
struct MsgPackArchiveData {
    std::string version = "0.1";
    std::unordered_map<std::string, reflect::Generic> resources; 
    std::vector<EntitySegment> entities;
    struct PackedArchetype {
        std::vector<uint64_t> ids;
        std::vector<std::string> component_keys;
        std::vector<std::vector<std::vector<char>>> columns; 
    };
    std::vector<PackedArchetype> archetypes;
};

std::vector<char> MsgPackArchive::create(World& world, const SnapshotRegistry& reg) {
    MsgPackArchiveData data;
    for (const auto& [id, fac] : reg.resource_factories()) {
        void* ptr = world.get_resource_dynamic(id);
        if (ptr && fac.generic) data.resources[fac.key] = fac.generic->to_generic(ptr);
    }
    std::vector<uint64_t> ids;
    for (size_t i = 0; i < world.archetype_count(); ++i) {
        auto* arch = world.graph().get_archetype(i); if (!arch) continue;
        for (auto& ck : arch->table().chunks()) for (size_t k = 0; k < ck->count(); ++k) ids.push_back(ck->entity(k).id());
    }
    if (!ids.empty()) {
        std::sort(ids.begin(), ids.end());
        uint64_t start = ids[0]; uint32_t count = 1;
        for (size_t i = 1; i < ids.size(); ++i) {
            if (ids[i] == start + count) count++;
            else { data.entities.push_back({start, count}); start = ids[i]; count = 1; }
        }
        data.entities.push_back({start, count});
    }
    for (size_t i = 0; i < world.archetype_count(); ++i) {
        auto* arch = world.graph().get_archetype(i);
        if (!arch || arch->count() == 0) continue;
        std::vector<const ComponentFactory*> active;
        for (const auto& [fid, fac] : reg.factories()) if (arch->get_column_index(fid)) active.push_back(&fac);
        if (active.empty()) continue;
        MsgPackArchiveData::PackedArchetype pack;
        pack.ids.reserve(arch->count());
        for (auto& ck : arch->table().chunks()) for (size_t k = 0; k < ck->count(); ++k) pack.ids.push_back(ck->entity(k).id());
        
        for (auto* fac : active) {
            pack.component_keys.push_back(fac->key);
            std::vector<std::vector<char>> col_blobs;
            col_blobs.reserve(arch->count());
            auto col_idx = arch->get_column_index(fac->type_id);
            for (auto& chunk : arch->table().chunks()) {
                for (size_t k = 0; k < chunk->count(); ++k) {
                    void* ptr = chunk->component(*col_idx, k);
                    if (fac->msgpack) {
                        col_blobs.push_back(fac->msgpack->encode(ptr));
                    } else if (fac->generic) {
                        auto generic = fac->generic->to_generic(ptr);
                        col_blobs.push_back(reflect::write_msgpack(generic));
                    } else {
                        col_blobs.push_back({}); 
                    }
                }
            }
            pack.columns.push_back(std::move(col_blobs));
        }
        data.archetypes.push_back(std::move(pack));
    }
    return reflect::write_msgpack(data);
}

Result<void> MsgPackArchive::load(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, const std::vector<char>& buffer) {
    auto res_opt = reflect::read_msgpack<MsgPackArchiveData>(buffer);
    if (!res_opt) return Result<void>::err(ErrorCode::InternalError, "Failed to parse MsgPack archive");
    auto& data = *res_opt;
    for (const auto& seg : data.entities) {
        world.import_entities(static_cast<uint32_t>(seg.start), seg.count);
    }
    std::unordered_map<uint64_t, Entity> id_map;
    for (const auto& seg : data.entities) for(uint64_t i=0; i<seg.count; ++i) id_map[seg.start + i] = Entity(seg.start + i, 0);
    for (auto& pack : data.archetypes) {
        for (size_t i = 0; i < pack.ids.size(); ++i) {
            if (!id_map.contains(pack.ids[i])) continue;
            Entity e = id_map.at(pack.ids[i]);
            for (size_t c = 0; c < pack.component_keys.size(); ++c) {
                const ComponentFactory* fac_ptr = nullptr;
                for (const auto& [fid, fac] : reg.factories()) {
                    if (fac.key == pack.component_keys[c]) { fac_ptr = &fac; break; }
                }
                if (!fac_ptr) continue;
                const auto& blob = pack.columns[c][i];
                if (fac_ptr->msgpack) {
                    auto res = fac_ptr->msgpack->decode_cmd(cmd, e, blob);
                    if (res.is_err()) return res;
                } else if (fac_ptr->generic) {
                    auto generic = reflect::read_msgpack<reflect::Generic>(blob);
                    if(generic) {
                        auto res = fac_ptr->generic->from_generic_cmd(cmd, e, *generic);
                        if (res.is_err()) return res;
                    }
                }
            }
        }
    }
    return Result<void>::ok();
}

Result<void> MsgPackArchive::load_with_remap(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, const IDRemapRegistry& id_reg, const IDMapper& mapper, const std::vector<char>& buffer) {
    auto res_opt = reflect::read_msgpack<MsgPackArchiveData>(buffer);
    if (!res_opt) return Result<void>::err(ErrorCode::InternalError, "Failed to parse MsgPack archive");
    auto& data = *res_opt;
    for (auto& pack : data.archetypes) {
        for (size_t i = 0; i < pack.ids.size(); ++i) {
            Entity e = mapper.map(pack.ids[i]);
            for (size_t c = 0; c < pack.component_keys.size(); ++c) {
                const ComponentFactory* fac_ptr = nullptr;
                for (const auto& [fid, fac] : reg.factories()) {
                    if (fac.key == pack.component_keys[c]) { fac_ptr = &fac; break; }
                }
                if (!fac_ptr) continue;
                const auto& blob = pack.columns[c][i];
                auto perform_hook = [&](void* ptr) {
                    if (const auto* hook = id_reg.get_hook(fac_ptr->type_id)) {
                        if (ptr) (*hook)(ptr, mapper);
                    }
                };
                if (fac_ptr->msgpack) {
                    auto res = fac_ptr->msgpack->decode_cmd(cmd, e, blob);
                    if (res.is_err()) return res;
                    perform_hook(cmd.last_payload());
                } else if (fac_ptr->generic) {
                    auto generic = reflect::read_msgpack<reflect::Generic>(blob);
                    if(generic) {
                        auto res = fac_ptr->generic->from_generic_cmd(cmd, e, *generic);
                        if (res.is_err()) return res;
                        perform_hook(cmd.last_payload());
                    }
                }
            }
        }
    }
    return Result<void>::ok();
}

} // namespace elysia::archive