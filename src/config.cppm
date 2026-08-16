module;
#include <cstddef>
#include <bit>

export module elysia.config;

export namespace elysia {

/**
 * @brief Global ECS Configuration.
 * 
 * MANDATE: ChunkSize MUST be a power of two (e.g., 512, 1024, 2048) or -1.
 * This ensures all addressing logic is optimized to bitwise shifts and masks.
 */
struct DefaultConfig {
    // Using int to allow -1 semantic for Monolithic mode
    static constexpr int ChunkSize = 2048;

    // Static enforcement at the definition site
    static_assert(ChunkSize == -1 || (ChunkSize > 0 && (ChunkSize & (ChunkSize - 1)) == 0),
        "Elysia Config Error: ChunkSize must be a power of two or -1 (Monolithic).");
};

} // namespace elysia
