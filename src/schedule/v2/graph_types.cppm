module;
#include <cstdint>
#include <unordered_map>
export module elysia.schedule:graph_types;

import elysia.entity;

export namespace elysia::schedule {

    /**
     * @brief Unique identifier for a node in the execution DAG.
     */
    struct GraphNode {
        enum Type : uint8_t { System, SetStart, SetEnd } type;
        entity_t entity;

        bool operator==(const GraphNode& o) const { 
            return type == o.type && entity == o.entity; 
        }
    };

} // namespace elysia::schedule
 
// 🌸 Globally specialize hash for GraphNode in the main module
// This ensures any consumer of elysia.schedule sees the specialization.
 template<> struct std::hash<elysia::schedule::GraphNode> {
    size_t operator()(const elysia::schedule::GraphNode& gn) const noexcept {
        return (std::hash<uint32_t>{}(gn.entity.id()) << 2) | (size_t)gn.type;
    }
};
