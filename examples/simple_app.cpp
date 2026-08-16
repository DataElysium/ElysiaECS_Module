/**
 * @file simple_app.cpp
 * @brief Minimal Elysia ECS demo — WASD move + render.
 * Copy the exact pattern from boids_app.cpp.
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

import elysia;
import elysia.query;

using namespace elysia;

constexpr int W = 1280, H = 720;

struct Pos { float x, y; };
struct Vel { float x, y; };
struct Body { float radius; Color color; };
struct Player {};

/* ── InputSystem: WASD drives velocity ONLY for player ── */
struct InputSystem {
    Query<Pos, Vel, With<Player>> q;
    void operator()(WorldView w) {
        w.update_query(q);
        q.each(w.raw(), [&](Pos& p, Vel& v) {
            (void)p;
            float ax = 0, ay = 0;
            if (IsKeyDown(KEY_W)) ay -= 1;
            if (IsKeyDown(KEY_S)) ay += 1;
            if (IsKeyDown(KEY_A)) ax -= 1;
            if (IsKeyDown(KEY_D)) ax += 1;
            float len = std::sqrt(ax*ax + ay*ay);
            if (len > 0) { v.x = ax/len * 4.0f; v.y = ay/len * 4.0f; }
        });
    }
};

/* ── MovementSystem: p += v, wrap ── */
struct MovementSystem {
    Query<Pos, Vel> q;
    void operator()(WorldView w) {
        w.update_query(q);
        q.each(w.raw(), [&](Pos& p, Vel& v) {
            p.x += v.x; p.y += v.y;
            if (p.x < 0) p.x += W; if (p.x > W) p.x -= W;
            if (p.y < 0) p.y += H; if (p.y > H) p.y -= H;
        });
    }
};

/* ── RenderSystem: draw circles ── */
struct RenderSystem {
    Query<const Pos, const Body> q;
    void operator()(WorldView w) {
        w.update_query(q);
        BeginDrawing();
        ClearBackground(DARKGRAY);
        q.each(w.raw(), [&](const Pos& p, const Body& b) {
            DrawCircle(p.x, p.y, b.radius, b.color);
        });
        DrawFPS(10, 10);
        EndDrawing();
    }
};

int main() {
    App app;

    InitWindow(W, H, "Elysia Simple Demo");
    SetTargetFPS(60);

    app.system("Input").run(InputSystem()).build();
    app.system("Move").after("Input").run(MovementSystem()).build();
    app.system("Render").after("Move").run(RenderSystem()).build();

    // Spawn player + some NPCs
    {
        CommandBuffer cmd(&app.world().index());
        // Player (green)
        Entity e = app.world().spawn().entity;
        cmd.insert(e, Pos{W/2.0f, H/2.0f});
        cmd.insert(e, Vel{0, 0});
        cmd.insert(e, Body{15, GREEN});
        cmd.insert(e, Player{});

        // NPCs (blue)
        for (int i = 0; i < 10; i++) {
            Entity n = app.world().spawn().entity;
            cmd.insert(n, Pos{(float)(rand() % W), (float)(rand() % H)});
            cmd.insert(n, Vel{(float)(rand() % 3 - 1), (float)(rand() % 3 - 1)});
            cmd.insert(n, Body{10, BLUE});
        }
        app.world().submit(cmd);
    }

    while (!WindowShouldClose()) {
        app.update();
    }

    CloseWindow();
    return 0;
}
