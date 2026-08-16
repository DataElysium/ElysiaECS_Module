module;
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <cstdint>

export module elysia.archive:aurora;

import :model;
import :registry;
import :utils;
import :resolver;
import :codec.csv;
import :codec.columnar;
import :codec.raw;
import elysia.reflect_wrapper;
import elysia.result;
import elysia.world;
import elysia.entity;
import elysia.storage;

export namespace elysia::archive {

class ELYSIA_ARCHIVE_API AuroraArchive {
public:
    struct Config {
        enum class Format { 
            Columnar, // Path A: Structured (using 'data' field)
            Csv,      // Path B: Blob (using 'blob' field, text)
            Raw       // Path B: Blob (using 'blob' field, binary)
        };
        Format format = Format::Csv;
    };

    static WorldArchive create(World& world, const SnapshotRegistry& reg, Config config);
    static WorldArchive create(World& world, const SnapshotRegistry& reg) {
        return create(world, reg, Config{});
    }
};

// Loaders
ELYSIA_ARCHIVE_API Result<void> load_aurora(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, const WorldArchive& archive);
ELYSIA_ARCHIVE_API Result<void> load_aurora_with_remap(World& world, CommandBuffer& cmd, 
                                    const SnapshotRegistry& reg, 
                                    const IDRemapRegistry& id_reg,
                                    const IDMapper& mapper,
                                    const WorldArchive& archive);

// Implementation

WorldArchive AuroraArchive::create(World& world, const SnapshotRegistry& reg, Config config) {
    WorldArchive archive;
    for (const auto& [id, fac] : reg.resource_factories()) {
        void* ptr = world.get_resource_dynamic(id);
        if (ptr && fac.generic) {
            archive.resources[fac.key] = reflect::write_json(fac.generic->to_generic(ptr));
        }
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
            else { archive.entities.push_back({start, count}); start = ids[i]; count = 1; }
        }
        archive.entities.push_back({start, count});
    }
    int arch_counter = 0;
    for (size_t i = 0; i < world.archetype_count(); ++i) {
        auto* arch = world.graph().get_archetype(i);
        if (!arch || arch->count() == 0) continue;
        std::vector<const ComponentFactory*> active;
        for (const auto& [fid, fac] : reg.factories()) if (arch->get_column_index(fid)) active.push_back(&fac);
        if (active.empty()) continue;
        std::string res_name = "arch_" + std::to_string(arch_counter++);
        ResourceEntry re; ArchetypeBlob blob_def;
        blob_def.name = res_name; blob_def.source_uri = "embed://" + res_name;
        blob_def.table_str = std::to_string(arch->count()); // 🌸 Stash count in table_str for RawTableCodec
        for (auto* fac : active) blob_def.components.push_back(fac->key);
        
        if (config.format == Config::Format::Csv) {
            re.format = "csv"; 
            re.encoding = "utf-8";
            re.blob = export_archetype_to_csv(*arch, active);
        } else if (config.format == Config::Format::Raw) {
            re.format = "raw_v3";
            re.encoding = "base64"; 
            auto bytes = RawTableCodec::export_archetype(*arch, active);
            re.blob = base64_encode(std::string(bytes.begin(), bytes.end()));
        } else {
            auto col_data = export_archetype_to_columnar(*arch, active);
            re.format = "columnar"; 
            re.encoding = "raw"; 
            auto json = reflect::write_json(col_data);
            auto generic_opt = reflect::read_json<reflect::Generic>(json);
            if (generic_opt) re.data = *generic_opt;
        }
        archive.embed[res_name] = std::move(re);
        archive.archetypes.push_back(std::move(blob_def));
    }
    return archive;
}

ELYSIA_ARCHIVE_API Result<void> load_aurora(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, const WorldArchive& archive) {
    for (const auto& seg : archive.entities) world.import_entities(static_cast<uint32_t>(seg.start), seg.count);
    std::unordered_map<uint64_t, Entity> id_map;
    for (const auto& seg : archive.entities) for(uint64_t i=0; i<seg.count; ++i) id_map[seg.start + i] = Entity(seg.start + i, 0);
    
    for (const auto& blob_def : archive.archetypes) {
        auto entry_res = ResolverRegistry::instance().resolve(blob_def.source_uri, archive);
        if (entry_res.is_err()) return Result<void>::err(entry_res.error().code, entry_res.error().message);
        const auto& entry = entry_res.unwrap();

        std::vector<const ComponentFactory*> active_factories;
        for (const auto& c_key : blob_def.components) {
            for (const auto& [id, fac] : reg.factories()) {
                if (fac.key == c_key) { active_factories.push_back(&fac); break; }
            }
        }

        if (entry.format == "csv") {
            auto payload_res = decode_payload(entry);
            if (payload_res.is_err()) return Result<void>::err(payload_res.error().code, payload_res.error().message);
            std::string csv_str(payload_res.unwrap().begin(), payload_res.unwrap().end());
            auto r = load_from_csv_flattened(world, cmd, reg, csv_str, id_map); 
            if (r.is_err()) return r;
        } else if (entry.format == "raw_v3") {
            auto payload_res = decode_payload(entry);
            if (payload_res.is_err()) return Result<void>::err(payload_res.error().code, payload_res.error().message);
            uint32_t count = static_cast<uint32_t>(std::stoul(blob_def.table_str));
            auto r = RawTableCodec::load(world, cmd, active_factories, payload_res.unwrap(), count, id_map);
            if (r.is_err()) return r;
        } else if (entry.format == "columnar") {
            auto r = load_from_columnar(world, cmd, reg, entry, id_map); 
            if (r.is_err()) return r;
        }
    }
    return Result<void>::ok();
}

ELYSIA_ARCHIVE_API Result<void> load_aurora_with_remap(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, const IDRemapRegistry& id_reg, const IDMapper& mapper, const WorldArchive& archive) {
    for (const auto& blob_def : archive.archetypes) {
        auto entry_res = ResolverRegistry::instance().resolve(blob_def.source_uri, archive);
        if (entry_res.is_err()) return Result<void>::err(entry_res.error().code, entry_res.error().message);
        const auto& entry = entry_res.unwrap();

        if (entry.format == "csv") {
            auto payload_res = decode_payload(entry);
            if (payload_res.is_err()) return Result<void>::err(payload_res.error().code, payload_res.error().message);
            std::string csv_str(payload_res.unwrap().begin(), payload_res.unwrap().end());
            auto r = load_from_csv_flattened(world, cmd, reg, csv_str, id_reg, mapper); 
            if (r.is_err()) return r;
        } else if (entry.format == "columnar") {
            auto r = load_from_columnar(world, cmd, reg, entry, id_reg, mapper); 
            if (r.is_err()) return r;
        }
    }
    return Result<void>::ok();
}

} // namespace elysia::archive