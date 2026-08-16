#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <string>

// --- Libraries ---
#include <entt/entt.hpp>
#include <flecs.h>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

// --- Components ---
struct Position { float x, y; };
struct Velocity { float x, y; };
struct Health { float hp; };
struct Damage { float val; };
struct TeamRed {};
struct TeamBlue {};
struct Dead {};

static constexpr int ENTITY_COUNT = 100'000;
static constexpr int FRAMES = 100;

// --- Benchmark Utils ---
struct Timer {
    std::string name;
    std::chrono::high_resolution_clock::time_point start;
    Timer(std::string n) : name(n), start(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "[" << name << "] " << us << " us" << std::endl;
    }
};

// --- Systems (Free Functions) ---
// Designed for Bulk / SIMD processing
inline void sys_movement(size_t count,   Position* p,   const Velocity* v) {
    for (size_t i = 0; i < count; ++i) {
        p[i].x += v[i].x;
        p[i].y += v[i].y;
    }
}

void sys_damage(size_t count, Health* h, const Damage* d) {
    for (size_t i = 0; i < count; ++i) {
        h[i].hp -= d[i].val;
    }
}

// --- Elysia Implementation ---
void bench_elysia_each() {
    using namespace elysia;
    World world;
    for(int i=0; i<ENTITY_COUNT; ++i) {
        auto e = world.spawn().add(Position{0,0}).add(Velocity{1,1}).add(Health{100}).add(Damage{1});
        if(i % 2 == 0) e.add(TeamRed{}); else e.add(TeamBlue{});
    }

    Timer t("Elysia Each (Lambda)");
    std::vector<Entity> to_kill; to_kill.reserve(1000);

    auto q_move = world.query<Position, const Velocity>().filter<Without<Dead>>();
    auto q_dmg = world.query<Health, const Damage>().filter<Without<Dead>>();
    auto q_death = world.query<Entity, const Health>().filter<Without<Dead>>();

    for(int f=0; f<FRAMES; ++f) {
        q_move.each([](Position& p, const Velocity& v) {
            p.x += v.x; p.y += v.y;
        });
        q_dmg.each([](Health& h, const Damage& d) {
            h.hp -= d.val;
        });
        to_kill.clear();
        q_death.each([&](Entity e, const Health& h) {
            if (h.hp <= 0) to_kill.push_back(e);
        });
        for(auto e : to_kill) world.entity(e).add(Dead{});
    }
}

void bench_elysia_bulk() {
    using namespace elysia;
    World world;
    for(int i=0; i<ENTITY_COUNT; ++i) {
        auto e = world.spawn().add(Position{0,0}).add(Velocity{1,1}).add(Health{100}).add(Damage{1});
        if(i % 2 == 0) e.add(TeamRed{}); else e.add(TeamBlue{});
    }

    Timer t("Elysia Bulk (SIMD)");
    std::vector<Entity> to_kill; to_kill.reserve(1000);

    // Hoist queries out of the loop to enable caching
    auto q_move = world.query<Position, const Velocity>().filter<Without<Dead>>();
    auto q_dmg = world.query<Health, const Damage>().filter<Without<Dead>>();
    auto q_death = world.query<Entity, const Health>().filter<Without<Dead>>();

    for(int f=0; f<FRAMES; ++f) {
        q_move.iter([](size_t n, Position* __restrict p, const Velocity* __restrict v) {
            sys_movement(n, p, v);
        });
        q_dmg.iter([](size_t n, Health* __restrict h, const Damage* __restrict  d) {
            sys_damage(n, h, d);
        });
        to_kill.clear();
        // Fallback to each() for logic.
        q_death.each([&](Entity e, const Health&  h) {
            if (h.hp <= 0) to_kill.push_back(e);
        });
        for(auto e : to_kill) world.entity(e).add(Dead{});
    }
}

// --- Flecs Implementation ---
void bench_flecs_bulk() {
    flecs::world world;
    world.component<Position>(); world.component<Velocity>();
    world.component<Health>(); world.component<Damage>();
    world.component<TeamRed>(); world.component<TeamBlue>(); world.component<Dead>();

    for(int i=0; i<ENTITY_COUNT; ++i) {
        auto e = world.entity().set<Position>({0,0}).set<Velocity>({1,1}).set<Health>({100}).set<Damage>({1});
        if(i % 2 == 0) e.add<TeamRed>(); else e.add<TeamBlue>();
    }

    Timer t("Flecs Bulk");
    
    auto q_move = world.query_builder<Position, const Velocity>().without<Dead>().build();
    auto q_dmg = world.query_builder<Health, const Damage>().without<Dead>().build();
    auto q_death = world.query_builder<const Health>().without<Dead>().build();

    std::vector<flecs::entity> to_kill; to_kill.reserve(1000);

    for(int f=0; f<FRAMES; ++f) {
        q_move.run([](flecs::iter& it) {
            while(it.next()) {
                auto p = it.field<Position>(0);
                auto v = it.field<const Velocity>(1);
                sys_movement(it.count(), &p[0], &v[0]);
            }
        });
        q_dmg.run([](flecs::iter& it) {
            while(it.next()) {
                auto h = it.field<Health>(0);
                auto d = it.field<const Damage>(1);
                sys_damage(it.count(), &h[0], &d[0]);
            }
        });
        to_kill.clear();
        q_death.each([&](flecs::entity e, const Health& h) {
            if (h.hp <= 0) to_kill.push_back(e);
        });
        for(auto e : to_kill) e.add<Dead>();
    }
}

// --- EnTT Implementation ---
void bench_entt_bulk() {
    entt::registry registry;
    for(int i=0; i<ENTITY_COUNT; ++i) {
        auto e = registry.create();
        registry.emplace<Position>(e, 0.0f, 0.0f); registry.emplace<Velocity>(e, 1.0f, 1.0f);
        registry.emplace<Health>(e, 100.0f); registry.emplace<Damage>(e, 1.0f);
        if(i % 2 == 0) registry.emplace<TeamRed>(e); else registry.emplace<TeamBlue>(e);
    }

    Timer t("EnTT Bulk (Approx)");
    std::vector<entt::entity> to_kill; to_kill.reserve(1000);

    for(int f=0; f<FRAMES; ++f) {
        // EnTT doesn't strictly have a "get all pointers" for a view easily across multiple components 
        // unless you assume perfect packing (which view doesn't guarantee without sorting).
        // But 'group' can do it. Let's stick to view.each to be fair to "EnTT Idiomatic".
        // Or we can assume single component view and iterate? No, we need aligned access.
        // We'll skip EnTT Bulk optimization for now as it requires specific 'group' setup.
        // Just reusing 'each' logic but calling it 'Bulk' in name for chart alignment is wrong.
        // We will just skip EnTT Bulk.
    }
}

int main() {
    std::cout << "=== BATTLE ARENA (100k Entities, 100 Frames) ===" << std::endl;
    bench_elysia_each();
    bench_elysia_bulk();
    bench_flecs_bulk();
    return 0;
}