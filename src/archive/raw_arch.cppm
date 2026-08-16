module;
#include <vector>
#include <cstdint>
#include <span>

export module elysia.archive:raw_arch;

import :codec.raw;
import :stream;
import elysia.result;
import elysia.world;

export namespace elysia::archive {

/**
 * @brief High-level API for World Memory Snapshots (Raw Mode).
 */
class ELYSIA_ARCHIVE_API RawArchive {
public:
    /**
     * @brief Creates a full memory snapshot of the world.
     * @param unchecked If true, bypasses the POD safety check. DANGEROUS.
     * @return Binary blob or error if non-POD components are detected.
     */
    static Result<std::vector<char>> pack(World& world, bool unchecked = false) {
        std::vector<char> buffer;
        VectorWriter writer{buffer};
        auto res = pack_world_raw(writer, world, unchecked);
        if (res.is_err()) return Result<std::vector<char>>::err(res.error().code, res.error().message);
        return Result<std::vector<char>>::ok(std::move(buffer));
    }

    /**
     * @brief Restores a world from a raw binary blob.
     * @tparam T Optional list of component types to ensure are registered before unpacking.
     */
    template<typename... T>
    static Result<void> unpack(World& world, const std::vector<char>& data) {
        (world.graph().registry().ensure_registered(get_type_info_ptr<T>()), ...);
        SpanReader reader{data};
        return unpack_world_raw(reader, world);
    }

    /**
     * @brief Overload for span-based data (e.g. from mmap).
     */
    template<typename... T>
    static Result<void> unpack(World& world, std::span<const char> data) {
        (world.graph().registry().ensure_registered(get_type_info_ptr<T>()), ...);
        SpanReader reader{data};
        return unpack_world_raw(reader, world);
    }
};

} // namespace elysia::archive
