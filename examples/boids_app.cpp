/**
 * @file boids_app.cpp
 * @brief High-performance Boids (Flocking) simulation using Elysia ECS and Raylib.
 *
 * Demonstrates the Functor System pattern: stateful systems with member Query storage,
 * persisting state (spatial grid cache) across frames for performance.
 *
 * Boids Algorithm:
 *   Each boid follows three simple rules based on nearby neighbors within a
 *   spatial hash grid (40px cells, 9-neighborhood):
 *     - Separation:   Steer away from boids that are too close (radius² = 100)
 *     - Alignment:    Steer toward the average heading of neighbors (range² = 2500)
 *     - Cohesion:     Steer toward the center of mass of neighbors (range² = 2500)
 *
 * Vel Component Semantics:
 *   Vel stores per-frame displacement (not continuous physics velocity).
 *   Speed clamp applies directly to the magnitude: MIN_SPEED = 2.0, MAX_SPEED = 4.0.
 *   Because Vel is per-frame, no dt multiplier is applied during integration.
 */

#if defined(_WIN32)
    #define NOGDI
    #define NOUSER
#endif
#include <fork_union.hpp>
#if defined(_WIN32)
    #undef Rectangle
    #undef CloseWindow
    #undef ShowCursor
    #undef DrawText
    #undef LoadImage
    #undef PlaySound
#endif
#include <raylib.h>
#include <vector>
#include <cmath>
#include <algorithm>
 
import elysia;
import elysia.query;

using namespace elysia;

// --- Constants ---
constexpr int SCREEN_WIDTH    = 1280;
constexpr int SCREEN_HEIGHT   = 720;
constexpr float CELL_SIZE     = 40.0f;           // Spatial hash grid cell size in pixels
constexpr int GRID_W          = SCREEN_WIDTH / (int)CELL_SIZE;
constexpr int GRID_H          = SCREEN_HEIGHT / (int)CELL_SIZE;

// Boids algorithm parameters
constexpr float MIN_SPEED     = 2.0f;             // Minimum per-frame displacement magnitude
constexpr float MAX_SPEED     = 4.0f;             // Maximum per-frame displacement magnitude
constexpr float SEPARATION_RADIUS_SQ = 100.0f;    // Separation radius squared (10²)
constexpr float VISUAL_RANGE_SQ    = 2500.0f;     // Visual range squared (50²)
constexpr float COHESION_WEIGHT   = 0.005f;       // Weight for center-of-mass steering
constexpr float ALIGNMENT_WEIGHT  = 0.05f;        // Weight for heading alignment
constexpr float SEPARATION_WEIGHT = 0.1f;         // Weight for collision avoidance
constexpr float DEG_PER_RAD       = 180.0f / 3.14159265358979323846f; // Degrees per radian

// --- Components ---
struct Pos { float x, y; };
struct Vel { float x, y; };
struct BoidTag {};

// --- Systems ---

/**
 * @brief BoidsRuleSystem: Manages flocking behaviors (Cohesion, Alignment, Separation).
 *
 * Demonstrates the Functor System pattern: a stateful system struct that persists
 * data (spatial hash grid + cached arrays) across frames, avoiding per-frame
 * allocations. Uses a spatial hash grid for O(n) neighbor lookups instead of O(n²).
 *
 * Spatial Hash Grid:
 *   The screen is divided into CELL_SIZE×CELL_SIZE cells. Each boid is placed into
 *   its corresponding cell during the collection pass. During the rules pass, each
 *   boid only examines neighbors in its own cell and the 8 adjacent cells (9-neighborhood).
 *
 * Order-Independent Self-Skip:
 *   Data is cached into flat arrays indexed by traversal order. Each boid compares its
 *   Entity ID against cached entities to skip itself, making this safe regardless of
 *   query traversal order (unlike index-based comparison which breaks if order changes).
 */
struct BoidsRuleSystem {
    AutoQuery<Entity, const Pos, Vel, With<BoidTag>> q;

    // Internal state: Spatial partitioning grid and cached data arrays
    std::vector<std::vector<size_t>> grid;
    std::vector<Entity> boid_entities;
    std::vector<float> pos_x, pos_y, vel_x, vel_y;

    BoidsRuleSystem() {
        grid.resize(GRID_W * GRID_H);
        // Pre-allocate data vectors to avoid per-frame reallocation overhead
        boid_entities.reserve(2048);
        pos_x.reserve(2048);
        pos_y.reserve(2048);
        vel_x.reserve(2048);
        vel_y.reserve(2048);
    }

    void operator()(WorldView w) {
        // Step 1: Clear grid and collect boid data into flat arrays for cache-friendly access
        for(auto& cell : grid) cell.clear();
        boid_entities.clear();
        pos_x.clear(); pos_y.clear(); vel_x.clear(); vel_y.clear();

        size_t idx = 0;
        q.each(w, [&](Entity e, const Pos& p, const Vel& v) {
            boid_entities.push_back(e);
            pos_x.push_back(p.x); pos_y.push_back(p.y);
            vel_x.push_back(v.x); vel_y.push_back(v.y);
            // Insert boid into the corresponding spatial hash cell
            int cx = std::clamp((int)(p.x / CELL_SIZE), 0, GRID_W - 1);
            int cy = std::clamp((int)(p.y / CELL_SIZE), 0, GRID_H - 1);
            grid[cy * GRID_W + cx].push_back(idx++);
        });

        // Step 2: Apply flocking rules — Cohesion, Alignment, Separation
        // Each boid examines neighbors in its 9-cell neighborhood (self + 8 adjacent)
        idx = 0;
        q.each(w, [&](Entity self, const Pos& p, Vel& v) {
            float sep_x = 0, sep_y = 0;   // Separation steering accumulator
            float ali_x = 0, ali_y = 0;   // Alignment steering accumulator
            float coh_x = 0, coh_y = 0;   // Cohesion steering accumulator
            int neighbors = 0;

            int cx = std::clamp((int)(p.x / CELL_SIZE), 0, GRID_W - 1);
            int cy = std::clamp((int)(p.y / CELL_SIZE), 0, GRID_H - 1);

            // Scan the 3×3 neighborhood centered on this boid's cell
            for(int dy = -1; dy <= 1; ++dy) {
                for(int dx = -1; dx <= 1; ++dx) {
                    int nx = cx + dx; int ny = cy + dy;
                    if(nx >= 0 && nx < GRID_W && ny >= 0 && ny < GRID_H) {
                        for(size_t cell_idx : grid[ny * GRID_W + nx]) {
                            if(boid_entities[cell_idx] == self) continue; // Skip self — order-independent
                            float dx_v = p.x - pos_x[cell_idx], dy_v = p.y - pos_y[cell_idx];
                            float d2 = dx_v * dx_v + dy_v * dy_v;
                            if(d2 < SEPARATION_RADIUS_SQ) {
                                // Separation: steer away from boids within close range
                                sep_x += dx_v; sep_y += dy_v;
                            } else if(d2 < VISUAL_RANGE_SQ) {
                                // Social range: alignment + cohesion with visible neighbors
                                ali_x += vel_x[cell_idx]; ali_y += vel_y[cell_idx];
                                coh_x += pos_x[cell_idx]; coh_y += pos_y[cell_idx];
                                neighbors++;
                            }
                        }
                    }
                }
            }

            // Apply weighted steering forces
            if(neighbors > 0) {
                v.x += (ali_x / neighbors - v.x) * ALIGNMENT_WEIGHT;
                v.y += (ali_y / neighbors - v.y) * ALIGNMENT_WEIGHT;
                v.x += (coh_x / neighbors - p.x) * COHESION_WEIGHT;
                v.y += (coh_y / neighbors - p.y) * COHESION_WEIGHT;
            }
            v.x += sep_x * SEPARATION_WEIGHT; v.y += sep_y * SEPARATION_WEIGHT;
            idx++;
        });
    }
};

/**
 * @brief MovementSystem: Euler integration with toroidal boundary wrapping and speed clamping.
 *
 * Integrates velocity into position each frame, then enforces three constraints:
 *   1. Toroidal wrapping: boids that exit one edge reappear on the opposite edge
 *   2. Speed maximum: velocity is clamped to MAX_SPEED to prevent runaway acceleration
 *   3. Speed minimum: velocity is boosted to MIN_SPEED to prevent boids from stalling
 *
 * Note: No dt multiplier is used because the Boids algorithm defines Vel as per-frame
 *       displacement (not continuous velocity). Speed thresholds apply directly to
 *       the raw magnitude of Vel.
 */
struct MovementSystem {
    AutoQuery<Pos, Vel> q;

    void operator()(WorldView w) {
        q.iter(w, [&](size_t count, Pos* __restrict p, Vel* __restrict v) {
            for (size_t i = 0; i < count; ++i) {
                // Euler integration: position += velocity (no dt — per-frame displacement)
                p[i].x += v[i].x; p[i].y += v[i].y;

                // Toroidal boundary wrapping: exit one edge, reappear on the opposite
                if (p[i].x < 0) p[i].x += SCREEN_WIDTH;
                else if (p[i].x > SCREEN_WIDTH) p[i].x -= SCREEN_WIDTH;
                if (p[i].y < 0) p[i].y += SCREEN_HEIGHT;
                else if (p[i].y > SCREEN_HEIGHT) p[i].y -= SCREEN_HEIGHT;

                // Speed clamping: normalize velocity and scale to stay within [MIN_SPEED, MAX_SPEED]
                float s = std::sqrt(v[i].x * v[i].x + v[i].y * v[i].y);
                if (s > MAX_SPEED) {
                    // Normalize and scale up to MAX_SPEED
                    float r = 1.0f / s; v[i].x = v[i].x * r * MAX_SPEED; v[i].y = v[i].y * r * MAX_SPEED;
                } else if (s < MIN_SPEED && s > 0.001f) {
                    // Normalize and scale up to MIN_SPEED (0.001f threshold avoids division by zero)
                    float r = 1.0f / s; v[i].x = v[i].x * r * MIN_SPEED; v[i].y = v[i].y * r * MIN_SPEED;
                }
            }
        });
    }
};

/**
 * @brief RenderSystem: Draws boids as triangles oriented along their velocity direction.
 *
 * Uses DrawPoly with 3 vertices (triangles) rotated to match each boid's heading.
 * The angle is computed from velocity via atan2, then converted from radians to degrees
 * for Raylib's angle convention. Rendered on a dark gray background with FPS overlay.
 */
struct RenderSystem {
    AutoQuery<const Pos, const Vel, With<BoidTag>> q;

    void operator()(WorldView w) {
        BeginDrawing();
        ClearBackground(DARKGRAY);
        q.each(w, [&](const Pos& p, const Vel& v) {
            // Orient triangle to face the direction of travel
            float angle = atan2f(v.y, v.x) * DEG_PER_RAD;
            DrawPoly(Vector2{p.x, p.y}, 3, 10.0f, angle, SKYBLUE);
        });
        DrawFPS(10, 10);
        EndDrawing();
    }
};

// --- Main ---
int main() {
    App app;

    // Step 1: Initialize the window at 60 FPS target
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Elysia Boids Example (Functor System)");
    SetTargetFPS(60);

    // Step 2: Register systems with execution order via Meta-ECS scheduler
    //   BoidsRule → Movement → Render (each depends on the previous)
    app.system("BoidsRule").run(BoidsRuleSystem()).build();
    app.system("Movement").after("BoidsRule").run(MovementSystem()).build();
    app.system("Render").after("Movement").run(RenderSystem()).build();

    // Step 3: Spawn initial boids using direct chain API (no CommandBuffer overhead)
    //   Random positions across the screen, random heading with initial speed ~3
    for(int i = 0; i < 1000; ++i) {
        float a = (float)(rand() % 360) * 0.017f;
        app.world().spawn()
            .add(Pos{(float)(rand() % SCREEN_WIDTH), (float)(rand() % SCREEN_HEIGHT)})
            .add(Vel{cosf(a) * 3, sinf(a) * 3})
            .add(BoidTag{});
    }

    // Step 4: Main loop — scheduler executes systems in registered order each frame
    while (!WindowShouldClose()) {
        app.update();
    }

    CloseWindow();
    return 0;
}
