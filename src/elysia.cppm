/**
 * @file elysia.cppm
 * @brief The unified entry point for Elysia ECS.
 * 
 * Simply 'import elysia;' to gain full access to the world, scheduler, 
 * meta-programming tools, and high-performance executors. Archive support is optional; import elysia.archive explicitly when enabled.
 */

export module elysia;

// --- Core Engine ---
export import elysia.entity;
export import elysia.world;
export import elysia.schedule;
export import elysia.app;
export import elysia.query;

// --- Infrastructure ---
export import elysia.meta;
export import elysia.result;
export import elysia.config;
export import elysia.observer;

// --- Advanced Access ---
// Note: elysia.storage is usually internal, but exported here for power users
// who need direct Archetype/Chunk access for manual optimization.
export import elysia.storage;
