module;
#include <string>
#include <vector>
#include <memory>
#include <string_view>
#include <fstream>
#include <sstream>
#include <unordered_map>

export module elysia.archive:resolver;

import :model;
import :utils;
import elysia.result;

export namespace elysia::archive {

/**
 * @brief Unified interface for resolving URIs into ResourceEntries.
 */
class IResourceResolver {
public:
    virtual ~IResourceResolver() = default;
    virtual std::string_view scheme() const = 0;
    virtual Result<ResourceEntry> resolve(const std::string& uri, const WorldArchive& manifest) = 0;
};

/**
 * @brief Resolver for "embed://" URIs.
 */
class EmbedResolver : public IResourceResolver {
public:
    std::string_view scheme() const override { return "embed"; }
    Result<ResourceEntry> resolve(const std::string& uri, const WorldArchive& manifest) override {
        if (!uri.starts_with("embed://")) return Result<ResourceEntry>::err(ErrorCode::InvalidOperation, "Invalid embed URI");
        std::string key = uri.substr(8);
        auto it = manifest.embed.find(key);
        if (it == manifest.embed.end()) return Result<ResourceEntry>::err(ErrorCode::NotFound, "Embedded resource not found: " + key);
        return Result<ResourceEntry>::ok(it->second);
    }
};

/**
 * @brief Resolver for "file://" URIs.
 */
class FileResolver : public IResourceResolver {
public:
    std::string_view scheme() const override { return "file"; }
    Result<ResourceEntry> resolve(const std::string& uri, const WorldArchive& manifest) override {
        if (!uri.starts_with("file://")) return Result<ResourceEntry>::err(ErrorCode::InvalidOperation, "Invalid file URI");
        std::string path = uri.substr(7);
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return Result<ResourceEntry>::err(ErrorCode::NotFound, "Failed to open file: " + path);
        
        std::stringstream ss; ss << ifs.rdbuf();
        ResourceEntry entry;
        entry.blob = ss.str();
        entry.encoding = "raw";
        
        // Basic format inference by extension
        if (path.ends_with(".csv")) entry.format = "csv";
        else if (path.ends_with(".bin")) entry.format = "columnar";
        else if (path.ends_with(".raw")) entry.format = "raw_v3";
        else entry.format = "unknown";
        
        return Result<ResourceEntry>::ok(entry);
    }
};

/**
 * @brief Manager for all registered resolvers.
 */
class ResolverRegistry {
public:
    static ResolverRegistry& instance() {
        static ResolverRegistry reg;
        return reg;
    }

    void register_resolver(std::unique_ptr<IResourceResolver> resolver) {
        std::string s(resolver->scheme());
        resolvers_[s] = std::move(resolver);
    }

    Result<ResourceEntry> resolve(const std::string& uri, const WorldArchive& manifest) {
        size_t pos = uri.find("://");
        if (pos == std::string::npos) return Result<ResourceEntry>::err(ErrorCode::InvalidOperation, "Malformed URI: " + uri);
        std::string scheme = uri.substr(0, pos);
        auto it = resolvers_.find(scheme);
        if (it == resolvers_.end()) return Result<ResourceEntry>::err(ErrorCode::NotFound, "No resolver for scheme: " + scheme);
        return it->second->resolve(uri, manifest);
    }

private:
    ResolverRegistry() {
        register_resolver(std::make_unique<EmbedResolver>());
        register_resolver(std::make_unique<FileResolver>());
    }
    std::unordered_map<std::string, std::unique_ptr<IResourceResolver>> resolvers_;
};

/**
 * @brief Middleware: Strips the transport encoding (e.g., Base64) to get raw bytes.
 */
inline Result<std::vector<char>> decode_payload(const ResourceEntry& entry) {
    if (!entry.blob) return Result<std::vector<char>>::err(ErrorCode::NotFound, "Resource blob is empty");
    
    if (entry.encoding == "base64") {
        std::string decoded = base64_decode(*entry.blob);
        std::vector<char> bytes(decoded.begin(), decoded.end());
        return Result<std::vector<char>>::ok(std::move(bytes));
    } else if (entry.encoding == "raw" || entry.encoding == "utf-8" || entry.encoding == "") {
        std::vector<char> bytes(entry.blob->begin(), entry.blob->end());
        return Result<std::vector<char>>::ok(std::move(bytes));
    }
    
    return Result<std::vector<char>>::err(ErrorCode::InvalidOperation, "Unsupported encoding: " + entry.encoding);
}

} // namespace elysia::archive
