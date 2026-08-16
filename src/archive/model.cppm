module;
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>
#include <cstdint>

export module elysia.archive:model;

import elysia.reflect_wrapper;

export namespace elysia::archive {

/**
 * @brief Represents a physical resource (Blob) with its metadata.
 */
struct ELYSIA_ARCHIVE_API ResourceEntry {
    std::string format;    // e.g., "csv", "columnar", "msgpack", "raw_v3"
    std::string encoding;  // e.g., "utf-8", "base64", "gzip", "raw"
    std::optional<reflect::Generic> data; // Structured high-level data
    std::optional<std::string> blob;      // Raw transport data (encoded)
};

struct ELYSIA_ARCHIVE_API EntitySegment {
    uint64_t start;
    uint32_t count;
};

struct ELYSIA_ARCHIVE_API ArchetypeBlob {
    std::string name;
    std::string table_str;
    std::vector<std::string> components;
    std::string source_uri;
};

struct ELYSIA_ARCHIVE_API WorldArchive {
    std::string version = "1.0";
    std::vector<EntitySegment> entities;
    std::vector<ArchetypeBlob> archetypes;
    std::unordered_map<std::string, ResourceEntry> embed; 
    
    // 🌸 Global Resources (Singletons) - Key is factory key, value is Generic
    std::unordered_map<std::string, reflect::Generic> resources;
    
    // 🌸 Metadata (Author, Date, Description)
    std::unordered_map<std::string, std::string> metadata;
};

} // namespace elysia::archive
