module;
#include <cstdint>
#include <compare>
#include <functional>
#include <limits>

export module elysia.entity;

export namespace elysia {

/**
 * @brief 64-bit Entity Identifier with Generation/Version support.
 * 
 * Layout:
 * [63...48]: Tag (16-bit) - Reserved for special flags / heterogeneous markers
 * [47...32]: Version (16-bit) - Generation count for ID reuse safety
 * [31...00]: ID (32-bit) - Index, supports up to 4 billion entities
 */
struct Entity {
    uint64_t value;

    // Constants
    static constexpr uint64_t ID_MASK      = 0x00000000FFFFFFFF;
    static constexpr uint64_t VERSION_MASK = 0x0000FFFF00000000;
    static constexpr uint64_t TAG_MASK     = 0xFFFF000000000000;
    
    static constexpr uint32_t ID_BITS      = 32;
    static constexpr uint32_t VERSION_BITS = 16;
    static constexpr uint32_t TAG_BITS     = 16;

    // Null Entity
    static constexpr uint64_t NULL_VALUE   = 0;

    // Constructors
    constexpr Entity() : value(NULL_VALUE) {}
    constexpr explicit Entity(uint64_t v) : value(v) {}
    constexpr Entity(uint32_t id, uint16_t ver, uint16_t tag = 0) {
        value = (static_cast<uint64_t>(tag) << (ID_BITS + VERSION_BITS)) |
                (static_cast<uint64_t>(ver) << ID_BITS) |
                static_cast<uint64_t>(id);
    }

    // Accessors
    [[nodiscard]] constexpr uint32_t id() const { 
        return static_cast<uint32_t>(value & ID_MASK); 
    }
    
    [[nodiscard]] constexpr uint16_t version() const { 
        return static_cast<uint16_t>((value & VERSION_MASK) >> ID_BITS); 
    }
    
    [[nodiscard]] constexpr uint16_t tag() const { 
        return static_cast<uint16_t>((value & TAG_MASK) >> (ID_BITS + VERSION_BITS)); 
    }

    // Checks
    [[nodiscard]] constexpr bool is_valid() const { return value != NULL_VALUE; }
    [[nodiscard]] constexpr explicit operator bool() const { return is_valid(); }

    // Comparisons (Manual implementation to avoid <compare> issues)
    bool operator==(const Entity& other) const = default;
    auto operator<=>(const Entity&) const = delete; 
    
    // We only need == for hashmaps and maybe < for sets
    bool operator<(const Entity& other) const { return value < other.value; }
};

static_assert(sizeof(Entity) == 8, "Entity must be 64-bit");
static_assert(alignof(Entity) == 8, "Entity must be 64-bit aligned");

using entity_t = Entity;

} // namespace elysia

// Std Hash Specialization
export namespace std {
    template<> struct hash<elysia::Entity> {
        std::size_t operator()(const elysia::Entity& e) const noexcept {
            // Simple pass-through hash for uint64, relying on standard library's uint64 hash or identity
            // For a dense ID space, identity is often best.
            return static_cast<std::size_t>(e.value); 
        }
    };
}
