module;
#include <cassert>

export module elysia.world:iter;

import :world;
import :command;
import elysia.meta;
import elysia.entity;
import elysia.query;

export namespace elysia {

/**
 * @brief System Iteration Context.
 * The "Three-in-One" context for an ECS system.
 * 1. Read-only World access.
 * 2. System-private CommandBuffer for deferred mutations.
 * 3. Access to pre-baked QueryState.
 */
class Iter {
public:
    Iter(World* w, QueryState* q = nullptr, CommandBuffer* cmd = nullptr) 
        : world_(w), query_state_(q), cmd_(cmd) {}

    /// 🌸 Safe read-only access
    const World& world() const { return *world_; }
    
    /// Explicitly mutable access
    World& world_mut() { return *world_; }

    /// 🌸 System-exclusive CommandBuffer
    CommandBuffer& commands() { 
        assert(cmd_ && "Iter: CommandBuffer not available for this system!");
        return *cmd_; 
    }

    /// Access the pre-baked query of the system (if available).
    QueryState* query_state() { return query_state_; }

    /**
     * @brief Access the cached chunk views from the system's query.
     * Use this for manual chunk-level parallelization.
     */
    template<ValidQueryArg... Args>
    auto chunks() {
        assert(query_state_ && "Iter: This system has no pre-baked query!");
        // Re-bind to the typed query to access the high-perf chunked() API
        auto* typed_q = static_cast<Query<Args...>*>(query_state_);
        return typed_q->chunks();
    }

    /// Shortcut for world queries.
    template<ValidQueryArg... Args>
    auto query() {
        return world_->query<Args...>();
    }

private:
    World* world_;
    QueryState* query_state_;
    CommandBuffer* cmd_;
};

} // namespace elysia