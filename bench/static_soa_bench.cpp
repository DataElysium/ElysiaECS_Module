#include <iostream>
#include <vector>
#include <chrono>
#include <tuple>
#include <memory>

// Import Modules
import elysia.world;
import elysia.query;
import elysia.static_soa;
import elysia.static_soa.fixed;

// Use restrict macro
#if defined(_MSC_VER)
    #define ELYSIA_RESTRICT __restrict
#else
    #define ELYSIA_RESTRICT __restrict__
#endif

using namespace elysia;

// --- Components ---
struct Position { float x, y; };
struct Velocity { float x, y; };
struct Health { float hp; };
struct Damage { float val; };

// --- Constants ---
static constexpr int ENTITY_COUNT = 100'000;
static constexpr int FRAMES = 100;

// --- Timer ---
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

// --- Raw C++ Implementation ---
struct RawSoA {
    std::vector<Position> pos;
    std::vector<Velocity> vel;
    std::vector<Health> hp;
    std::vector<Damage> dmg;

    void resize(size_t n) {
        pos.resize(n); vel.resize(n); hp.resize(n); dmg.resize(n);
    }
};

void bench_raw_cpp() {
    RawSoA raw;
    raw.resize(ENTITY_COUNT);
    
    // Init
    for(int i=0; i<ENTITY_COUNT; ++i) {
        raw.pos[i] = {0,0}; raw.vel[i] = {1,1};
        raw.hp[i] = {100}; raw.dmg[i] = {1};
    }

    Timer t("Raw C++ Loop");
    for(int f=0; f<FRAMES; ++f) {
        // Movement
        size_t count = raw.pos.size();
        Position* ELYSIA_RESTRICT p = raw.pos.data();
        const Velocity* ELYSIA_RESTRICT v = raw.vel.data();
        
        for(size_t i=0; i<count; ++i) {
            p[i].x += v[i].x;
            p[i].y += v[i].y;
        }

        // Damage
        Health* ELYSIA_RESTRICT h = raw.hp.data();
        const Damage* ELYSIA_RESTRICT d = raw.dmg.data();
        for(size_t i=0; i<count; ++i) {
            h[i].hp -= d[i].val;
        }
    }
}

// --- Static SoA Implementation ---
void bench_static_soa() {
    StaticArchetype<Position, Velocity, Health, Damage> arch;
    arch.reserve(ENTITY_COUNT);

    for(int i=0; i<ENTITY_COUNT; ++i) {
        arch.push({0,0}, {1,1}, {100}, {1});
    }

    Timer t("Static SoA");
    for(int f=0; f<FRAMES; ++f) {
        // Manual iteration for sub-systems
        {
            arch.iter([](size_t count, Position* ELYSIA_RESTRICT p, const Velocity* ELYSIA_RESTRICT v, Health*, const Damage*) {
                for(size_t i=0; i<count; ++i) {
                    p[i].x += v[i].x;
                    p[i].y += v[i].y;
                }
            });
        }

        {
            arch.iter([](size_t count, Position*, const Velocity*, Health* ELYSIA_RESTRICT h, const Damage* ELYSIA_RESTRICT d) {
                for(size_t i=0; i<count; ++i) {
                    h[i].hp -= d[i].val;
                }
            });
        }
    }
}

// --- Fixed SoA Implementation (SIMD Aligned) ---
// Simulating "Struct Flattening" manually:
// Instead of storing Position(x,y), we store Float, Float (Px, Py)
// This guarantees perfect alignment for X and Y arrays separately.
void bench_fixed_soa() {
    // Components: Px, Py, Vx, Vy, Hp, Dmg
    using ArchType = FixedArchetype<ENTITY_COUNT, 32, float, float, float, float, float, float>;
    auto arch_ptr = std::make_unique<ArchType>();
    auto& arch = *arch_ptr;

    for(int i=0; i<ENTITY_COUNT; ++i) {
        arch.push(0.0f, 0.0f, 1.0f, 1.0f, 100.0f, 1.0f);
    }

    Timer t("Fixed SoA (32-byte Align + Unrolled)");
    for(int f=0; f<FRAMES; ++f) {
        // Full iteration with pointer unpacking
        arch.iter([](size_t n, 
            float* ELYSIA_RESTRICT px, float* ELYSIA_RESTRICT py, 
            float* ELYSIA_RESTRICT vx, float* ELYSIA_RESTRICT vy, 
            float* ELYSIA_RESTRICT hp, float* ELYSIA_RESTRICT dmg) 
        {
            // Compiler sees:
            // 1. Pointers are restricted (no overlap)
            // 2. Arrays are 32-byte aligned (AVX2 friendly)
            // 3. Size is constant known at compile time (ENTITY_COUNT) -> Unrolling!
            
            // Movement
            for(size_t i=0; i<n; ++i) {
                px[i] += vx[i];
                py[i] += vy[i];
            }
            
            // Damage
            for(size_t i=0; i<n; ++i) {
                hp[i] -= dmg[i];
            }
        });
    }
}

// --- Dynamic Elysia Implementation ---
void bench_dynamic_elysia() {
    World world;
    for(int i=0; i<ENTITY_COUNT; ++i) {
        world.spawn().add(Position{0,0}).add(Velocity{1,1}).add(Health{100}).add(Damage{1});
    }

    // Hoist queries
    auto q_move = world.query<Position, const Velocity>();
    auto q_dmg = world.query<Health, const Damage>();

    Timer t("Dynamic Elysia");
    for(int f=0; f<FRAMES; ++f) {
        q_move.iter([](size_t n, Position* ELYSIA_RESTRICT p, const Velocity* ELYSIA_RESTRICT v) {
            for(size_t i=0; i<n; ++i) {
                p[i].x += v[i].x;
                p[i].y += v[i].y;
            }
        });
        q_dmg.iter([](size_t n, Health* ELYSIA_RESTRICT h, const Damage* ELYSIA_RESTRICT d) {
            for(size_t i=0; i<n; ++i) {
                h[i].hp -= d[i].val;
            }
        });
    }
}

int main() {
    std::cout << "=== STATIC vs DYNAMIC vs RAW (100k, 100 frames) ===" << std::endl;
    bench_raw_cpp();
    bench_static_soa();
    bench_fixed_soa();
    bench_dynamic_elysia();
    return 0;
}