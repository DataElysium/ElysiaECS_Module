/**
 * @file spacewar_app.cpp
 * @brief Spacewar! — the classic 1962 game, built with Elysia ECS.
 *
 * Two players battle around a gravity star.
 *   Player 1: W (thrust)  A/D (rotate)  Space (fire)
 *   Player 2: ↑ (thrust)  ←/→ (rotate)  Enter (fire)
 *
 * Architecture (serial pipeline):
 *   Input → Gravity → Movement → Bullet → Collision → Render
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
#include <cmath>
#include <cstdio>
#include <raylib.h>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

import elysia;
import elysia.query;

using namespace elysia;

/* ========================================================================= */
/*  Constants                                                                */
/* ========================================================================= */

constexpr int SW = 1280, SH = 720;
constexpr float G = 800.0f;          // gravitational constant
constexpr float STAR_MASS = 200.0f;  // mass of central star
constexpr float THRUST = 200.0f;     // ship acceleration
constexpr float ROTATE_SPD = 4.0f;   // radians per second
constexpr float BULLET_SPD = 500.0f; // bullet initial speed
constexpr int BULLET_LIFE = 120;     // frames before bullet expires
constexpr int FIRE_COOLDOWN = 15;
constexpr float MAX_FUEL = 3000.0f;
constexpr float FUEL_BURN = 35.0f; // fuel per second of thrust
constexpr float SHIP_RADIUS = 12.0f;
constexpr float BULLET_RADIUS = 3.0f;
constexpr float STAR_RADIUS = 25.0f;

/* ========================================================================= */
/*  Components                                                               */
/* ========================================================================= */

struct Pos {
  float x, y;
};
struct Vel {
  float x, y;
};
struct Angle {
  float a;
};
struct Body {
  float radius;
  Color color;
};
struct Fuel {
  float amount;
};
struct Lifetime {
  int frames;
};

// Tags
struct Ship {};
struct Bullet {};
struct Star {};
struct Player1 {};
struct Player2 {};

/* ========================================================================= */
/*  Resources                                                                */
/* ========================================================================= */

struct GameState {
  bool p1_alive = true;
  bool p2_alive = true;
  int p1_score = 0;
  int p2_score = 0;
  bool game_over = false;
  float restart_timer = 0;
};

/* ========================================================================= */
/*  Systems                                                                  */
/* ========================================================================= */

/* ── InputSystem: keyboard → ship velocity / angle / fire ── */

struct InputSystem {
  Query<Pos, Vel, Angle, Fuel, With<Player1>> p1q;
  Query<Pos, Vel, Angle, Fuel, With<Player2>> p2q;
  int cd1 = 0, cd2 = 0;

  void operator()(World *w, CommandBuffer *cmd) {
    float dt = GetFrameTime();
    w->update_query(p1q);
    w->update_query(p2q);
    int n1 = 0;
    p1q.each(w, [&](Pos &p, Vel &v, Angle &a, Fuel &f) {
      n1++;

      if (IsKeyDown(KEY_A))
        a.a -= ROTATE_SPD * dt;
      if (IsKeyDown(KEY_D))
        a.a += ROTATE_SPD * dt;
      if (IsKeyDown(KEY_W) && f.amount > 0) {
        v.x += cosf(a.a) * THRUST * dt;
        v.y += sinf(a.a) * THRUST * dt;
        f.amount -= FUEL_BURN * dt;
        if (f.amount < 0)
          f.amount = 0;
      }
      if (IsKeyDown(KEY_SPACE) && cd1 <= 0) {
        cd1 = FIRE_COOLDOWN;
        float ox = p.x + cosf(a.a) * (SHIP_RADIUS + 5);
        float oy = p.y + sinf(a.a) * (SHIP_RADIUS + 5);
        Entity b = w->spawn().entity;
        cmd->insert(b, Pos{ox, oy});
        cmd->insert(
            b, Vel{cosf(a.a) * BULLET_SPD + v.x, sinf(a.a) * BULLET_SPD + v.y});
        cmd->insert(b, Angle{a.a});
        cmd->insert(b, Body{BULLET_RADIUS, YELLOW});
        cmd->insert(b, Lifetime{BULLET_LIFE});
        cmd->insert(b, Bullet{});
      }
    });
    if (cd1 > 0)
      cd1--;

    int n2 = 0;
    p2q.each(w, [&](Pos &p, Vel &v, Angle &a, Fuel &f) {
      n2++;
      if (IsKeyDown(KEY_LEFT))
        a.a -= ROTATE_SPD * dt;
      if (IsKeyDown(KEY_RIGHT))
        a.a += ROTATE_SPD * dt;
      if (IsKeyDown(KEY_UP) && f.amount > 0) {
        v.x += cosf(a.a) * THRUST * dt;
        v.y += sinf(a.a) * THRUST * dt;
        f.amount -= FUEL_BURN * dt;
        if (f.amount < 0)
          f.amount = 0;
      }
      if (IsKeyDown(KEY_ENTER) && cd2 <= 0) {

        cd2 = FIRE_COOLDOWN;
        float ox = p.x + cosf(a.a) * (SHIP_RADIUS + 5);
        float oy = p.y + sinf(a.a) * (SHIP_RADIUS + 5);
        Entity b = w->spawn().entity;
        cmd->insert(b, Pos{ox, oy});
        cmd->insert(
            b, Vel{cosf(a.a) * BULLET_SPD + v.x, sinf(a.a) * BULLET_SPD + v.y});
        cmd->insert(b, Angle{a.a});
        cmd->insert(b, Body{BULLET_RADIUS, YELLOW});
        cmd->insert(b, Lifetime{BULLET_LIFE});
        cmd->insert(b, Bullet{});
      }
    });
    if (cd2 > 0)
      cd2--;

    static int frame = 0;
    if (++frame % 60 == 0)
      printf("[Input] dt=%.4f  p1=%d  p2=%d\n", dt, n1, n2);
  }
};

/* ── GravitySystem: star pulls everything with Pos+Vel ── */

struct GravitySystem {
  Query<Pos, Vel, With<Ship>> ships;
  Query<Pos, Vel, With<Bullet>> bullets;
  Query<const Pos, With<Star>> star;

  void operator()(WorldView w) {
    float dt = GetFrameTime();
    w.update_query(star);
    w.update_query(ships);
    w.update_query(bullets);
    // Find star position
    float sx = SW / 2.0f, sy = SH / 2.0f;
    star.each(w.raw(), [&](const Pos &sp) {
      sx = sp.x;
      sy = sp.y;
    });

    auto apply_gravity = [&](Pos &p, Vel &v) {
      float dx = sx - p.x;
      float dy = sy - p.y;
      float d2 = dx * dx + dy * dy;
      if (d2 < STAR_RADIUS * STAR_RADIUS)
        d2 = STAR_RADIUS * STAR_RADIUS;
      float dist = std::sqrt(d2);
      float acc = G * STAR_MASS / d2 * dt;
      v.x += dx / dist * acc;
      v.y += dy / dist * acc;
    };

    ships.each(w.raw(), [&](Pos &p, Vel &v) { apply_gravity(p, v); });
    bullets.each(w.raw(), [&](Pos &p, Vel &v) { apply_gravity(p, v); });
  }
};

/* ── MovementSystem: p += v * dt, screen wrap ── */

struct MovementSystem {
  Query<Pos, Vel> q;

  void operator()(WorldView w) {
    float dt = GetFrameTime();
    w.update_query(q);
    q.each(w.raw(), [&](Pos &p, Vel &v) {
      p.x += v.x * dt;
      p.y += v.y * dt;
      if (p.x < -20)
        p.x += SW + 40;
      if (p.x > SW + 20)
        p.x -= SW + 40;
      if (p.y < -20)
        p.y += SH + 40;
      if (p.y > SH + 20)
        p.y -= SH + 40;
    });
  }
};

/* ── LifetimeSystem: lifetime countdown, despawn expired ── */

struct LifetimeSystem {
  Query<Lifetime, Entity> q;

  void operator()(WorldView w, CommandBuffer *cmd) {
    w.update_query(q);
    q.each(w.raw(), [&](Lifetime &lt, Entity e) {
      lt.frames--;
      if (lt.frames <= 0)
        cmd->despawn(e);
    });
  }
};

/* ── CollisionSystem: bullet ↔ ship ── */

struct CollisionSystem {
  struct BulletData {
    Entity e;
    float x, y;
  };
  Query<Pos, Entity, With<Bullet>> bq;
  Query<Pos, Entity, With<Ship>> sq;
  std::vector<BulletData> bullets;

  void operator()(World *w, CommandBuffer *cmd) {
    w->update_query(bq);
    w->update_query(sq);

    bullets.clear();
    bq.each(w, [&](Pos &bp, Entity be) {
      bullets.emplace_back(BulletData{be, bp.x, bp.y});
    });

    sq.each(w, [&](Pos &sp, Entity se) {
      for (size_t i = 0; i < bullets.size(); ++i) {
        float dx = sp.x - bullets[i].x;
        float dy = sp.y - bullets[i].y;
        if (dx * dx + dy * dy <
            (SHIP_RADIUS + BULLET_RADIUS) * (SHIP_RADIUS + BULLET_RADIUS)) {
          // Explosion particles
          for (int j = 0; j < 12; ++j) {
            Entity ep = w->spawn().entity;
            float ang = j * 0.5236f;
            cmd->insert(ep, Pos{sp.x, sp.y});
            cmd->insert(ep, Vel{cosf(ang) * 100, sinf(ang) * 100});
            cmd->insert(ep, Body{3, ColorAlpha(ORANGE, 0.7f)});
            cmd->insert(ep, Lifetime{45});
          }
          cmd->despawn(bullets[i].e);
          cmd->despawn(se);
          break;
        }
      }
    });
  }
};

/* ── RenderSystem: draw ships, bullets, star, particles ── */

struct RenderSystem {
  Query<Entity, const Pos, const Angle, const Body, With<Ship>> sq;
  Query<const Pos, const Angle, const Body, With<Bullet>> bq;
  Query<const Pos, const Body, With<Star>> stq;
  Query<const Pos, const Body, const Lifetime> expq; // explosions
  Query<Entity, With<Ship>, With<Player1>> p1_query;
  Query<Entity, With<Ship>, With<Player2>> p2_query;
  void operator()(WorldView w) {
    w.update_query(sq);
    w.update_query(bq);
    w.update_query(stq);
    w.update_query(expq);
    w.update_query(p1_query);
    w.update_query(p2_query);

    BeginDrawing();
    ClearBackground(BLACK);
    // Stars background
    DrawCircle(100, 80, 1, ColorAlpha(WHITE, 0.4f));
    DrawCircle(400, 150, 1, ColorAlpha(WHITE, 0.3f));
    DrawCircle(900, 100, 1, ColorAlpha(WHITE, 0.5f));
    DrawCircle(1100, 300, 1, ColorAlpha(WHITE, 0.3f));
    DrawCircle(200, 500, 1, ColorAlpha(WHITE, 0.4f));
    DrawCircle(700, 600, 1, ColorAlpha(WHITE, 0.3f));
    DrawCircle(1050, 650, 1, ColorAlpha(WHITE, 0.5f));
    DrawCircle(600, 350, 1, ColorAlpha(WHITE, 0.3f));

    // Central star
    stq.each(w.raw(), [&](const Pos &p, const Body &b) {
      DrawCircleGradient({p.x, p.y}, b.radius, YELLOW,
                         ColorAlpha(YELLOW, 0.1f));
      DrawCircleLines(p.x, p.y, b.radius + 2, ColorAlpha(YELLOW, 0.3f));
    });

    // Ships — drawn as triangles
    sq.each(w.raw(), [&](Entity e, const Pos &p, const Angle &a, const Body &b) {
      float r = b.radius;
      float x1 = p.x + cosf(a.a) * r;
      float y1 = p.y + sinf(a.a) * r;
      float x2 = p.x + cosf(a.a + 2.5f) * r * 0.7f;
      float y2 = p.y + sinf(a.a + 2.5f) * r * 0.7f;
      float x3 = p.x + cosf(a.a - 2.5f) * r * 0.7f;
      float y3 = p.y + sinf(a.a - 2.5f) * r * 0.7f;
      auto color = WHITE;
      if (w.entity(e).get<Player2>()) {
        color = SKYBLUE;
      } else {
        color = GREEN;
      }
      DrawTriangle({x1, y1}, {x2, y2}, {x3, y3}, b.color);
      DrawTriangleLines({x1, y1}, {x2, y2}, {x3, y3}, color);
    });

    // Bullets
    bq.each(w.raw(), [&](const Pos &p, const Angle &a, const Body &b) {
      (void)a;
      DrawCircle(p.x, p.y, b.radius, b.color);
    });

    // Explosions
    expq.each(w.raw(), [&](const Pos &p, const Body &b, const Lifetime &lt) {
      float alpha = (float)lt.frames / 45.0f;
      DrawCircle(p.x, p.y, b.radius, ColorAlpha(b.color, alpha));
    });

    // HUD
    DrawText("P1: WASD + Space", 10, 10, 14, GREEN);
    DrawText("P2: Arrows + Enter", 10, 28, 14, SKYBLUE);

    // Ship counts
    int s1 = 0, s2 = 0;

    bool p1_alive = !p1_query.empty();
    bool p2_alive = !p2_query.empty();

    DrawText(TextFormat("P1: %s", p1_alive ? "ALIVE" : "DEAD"), SW - 200, 10,
             16, GREEN);
    DrawText(TextFormat("P2: %s", p2_alive ? "ALIVE" : "DEAD"), SW - 200, 30,
             16, SKYBLUE);

    DrawFPS(10, SH - 30);
    EndDrawing();
  }
};

/* ── RespawnSystem: bring dead ships back after delay ── */

struct RespawnSystem {
  // Check if P1 is alive
  Query<Entity, With<Ship>, With<Player1>> c1;
  // Check if P2 is alive
  Query<Entity, With<Ship>, With<Player2>> c2;
  void operator()(World *w, CommandBuffer *cmd) {
    static float timer1 = 0, timer2 = 0;
    timer1 += GetFrameTime();
    timer2 += GetFrameTime();

    // Check if P1 is alive

    w->update_query(c1);
    bool p1_alive = false;
    c1.each(w, [&](Entity) { p1_alive = true; });

    if (!p1_alive && timer1 > 2.0f) {
      spawn_ship(w, cmd, true);
      timer1 = 0;
    }

    w->update_query(c2);
    bool p2_alive = false;
    c2.each(w, [&](Entity) { p2_alive = true; });

    if (!p2_alive && timer2 > 2.0f) {
      spawn_ship(w, cmd, false);
      timer2 = 0;
    }
  }

private:
  void spawn_ship(World *w, CommandBuffer *cmd, bool is_p1) {
    Entity e = w->spawn().entity;
    float x = is_p1 ? SW * 0.3f : SW * 0.7f;
    float y = SH * 0.5f + (is_p1 ? -40 : 40);
    float angle = is_p1 ? 0 : M_PI;
    Color color = is_p1 ? GREEN : SKYBLUE;

    cmd->insert(e, Pos{x, y});
    cmd->insert(e, Vel{0, 0});
    cmd->insert(e, Angle{angle});
    cmd->insert(e, Body{SHIP_RADIUS, color});
    cmd->insert(e, Fuel{MAX_FUEL});
    cmd->insert(e, Ship{});
    if (is_p1)
      cmd->insert(e, Player1{});
    else
      cmd->insert(e, Player2{});
  }
};

/* ========================================================================= */
/*  Main                                                                     */
/* ========================================================================= */

int main() {
  App app;

  InitWindow(SW, SH, "Spacewar! — Elysia ECS");
  SetTargetFPS(60);
  SetExitKey(KEY_ESCAPE);

  // Register systems (serial pipeline)
  app.scheduler().chain(
      app.system("Input").run(InputSystem()),
      app.system("Gravity").run(GravitySystem()),
      app.system("Movement").run(MovementSystem()),
      app.system("Lifetime").run(LifetimeSystem()),
      schedule::Sync,
      app.system("Collision").run(CollisionSystem()),
      schedule::Sync,
      app.system("Respawn").run(RespawnSystem()),
      app.system("Render").run(RenderSystem())
  );

  // Initial spawn
  {
    CommandBuffer cmd(&app.world().index());

    // Central star
    Entity star = app.world().spawn().entity;
    cmd.insert(star, Pos{SW / 2.0f, SH / 2.0f});
    cmd.insert(star, Body{STAR_RADIUS, YELLOW});
    cmd.insert(star, Star{});

    // Player 1 ship
    Entity p1 = app.world().spawn().entity;
    cmd.insert(p1, Pos{SW * 0.3f, SH * 0.5f});
    cmd.insert(p1, Vel{0, 0});
    cmd.insert(p1, Angle{0});
    cmd.insert(p1, Body{SHIP_RADIUS, GREEN});
    cmd.insert(p1, Fuel{MAX_FUEL});
    cmd.insert(p1, Ship{});
    cmd.insert(p1, Player1{});

    // Player 2 ship
    Entity p2 = app.world().spawn().entity;
    cmd.insert(p2, Pos{SW * 0.7f, SH * 0.5f});
    cmd.insert(p2, Vel{0, 0});
    cmd.insert(p2, Angle{3.14159f});
    cmd.insert(p2, Body{SHIP_RADIUS, SKYBLUE});
    cmd.insert(p2, Fuel{MAX_FUEL});
    cmd.insert(p2, Ship{});
    cmd.insert(p2, Player2{});

    app.world().submit(cmd);
  }

  while (!WindowShouldClose()) {
    app.update();
  }

  CloseWindow();
  return 0;
}
