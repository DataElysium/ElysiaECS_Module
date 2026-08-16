#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <string>
#include <iomanip>

#include <entt/entt.hpp>
#include <flecs.h>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;

// --- Components ---
struct C0 { float v = 1.0f; };
struct C1 { float v = 1.0f; };
struct C2 { float v = 1.0f; };
struct C3 { float v = 1.0f; };
struct C4 { float v = 1.0f; };
struct C5 { float v = 1.0f; };
struct C6 { float v = 1.0f; };
struct C7 { float v = 1.0f; };
struct C8 { float v = 1.0f; };
struct C9 { float v = 1.0f; };

class Timer {
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_;
public:
    Timer(std::string name) : name_(name), start_(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        double total_ms = total_ns / 1'000'000.0;
        std::cout << "[" << std::left << std::setw(20) << name_ << "] " 
                  << std::right << std::setw(10) << std::fixed << std::setprecision(3) << total_ms << " ms" << std::endl;
    }
};

void bench_elysia(int entity_count, const std::vector<int>& archetypes) {
    std::cout << "--- Elysia ECS ---" << std::endl;
    World world;
    CommandBuffer cmd(&world.index());
    
    {
        Timer t("Create (Direct)");
        for(int i=0; i<entity_count; ++i) {
            int mask = archetypes[i % archetypes.size()];
            auto view = world.spawn();
            if(mask & (1<<0)) view.add(C0{});
            if(mask & (1<<1)) view.add(C1{});
            if(mask & (1<<2)) view.add(C2{});
            if(mask & (1<<3)) view.add(C3{});
            if(mask & (1<<4)) view.add(C4{});
            if(mask & (1<<5)) view.add(C5{});
            if(mask & (1<<6)) view.add(C6{});
            if(mask & (1<<7)) view.add(C7{});
            if(mask & (1<<8)) view.add(C8{});
            if(mask & (1<<9)) view.add(C9{});
        }
    }
    std::cout << "Archetypes created (Direct): " << world.archetype_count() << std::endl;
    
    world = World(); 
    {
        Timer t("Create (Cmd)");
        for(int i=0; i<entity_count; ++i) {
            Entity e = world.spawn().entity;
            int mask = archetypes[i % archetypes.size()];
            cmd.spawn(e);
            if(mask & (1<<0)) cmd.insert(e, C0{});
            if(mask & (1<<1)) cmd.insert(e, C1{});
            if(mask & (1<<2)) cmd.insert(e, C2{});
            if(mask & (1<<3)) cmd.insert(e, C3{});
            if(mask & (1<<4)) cmd.insert(e, C4{});
            if(mask & (1<<5)) cmd.insert(e, C5{});
            if(mask & (1<<6)) cmd.insert(e, C6{});
            if(mask & (1<<7)) cmd.insert(e, C7{});
            if(mask & (1<<8)) cmd.insert(e, C8{});
            if(mask & (1<<9)) cmd.insert(e, C9{});
            if (i % 1000 == 0) world.submit(cmd);
        }
        world.submit(cmd);
    }
    std::cout << "Archetypes created (Cmd): " << world.archetype_count() << std::endl;

    {
        double total_v = 0;
        {
            Timer t("Iterate C0, C1");
            Query<const C0,const C1> q;
            world.update_query(q);
            q.each([&](const C0& c,const auto & ) {
                total_v += c.v;
            });
        }
        std::cout << "Sum C0.v: " << total_v << " (Verification)" << std::endl;
    }
}

void bench_entt(int entity_count, const std::vector<int>& archetypes) {
    std::cout << "--- EnTT ---" << std::endl;
    entt::registry registry;
    {
        Timer t("Create");
        for(int i=0; i<entity_count; ++i) {
            int mask = archetypes[i % archetypes.size()];
            auto e = registry.create();
            if(mask & (1<<0)) registry.emplace<C0>(e);
            if(mask & (1<<1)) registry.emplace<C1>(e);
            if(mask & (1<<2)) registry.emplace<C2>(e);
            if(mask & (1<<3)) registry.emplace<C3>(e);
            if(mask & (1<<4)) registry.emplace<C4>(e);
            if(mask & (1<<5)) registry.emplace<C5>(e);
            if(mask & (1<<6)) registry.emplace<C6>(e);
            if(mask & (1<<7)) registry.emplace<C7>(e);
            if(mask & (1<<8)) registry.emplace<C8>(e);
            if(mask & (1<<9)) registry.emplace<C9>(e);
        }
    }
    {
        double total_v = 0;
        {
            Timer t("Iterate C0, C1");
            auto view = registry.view<const C0,const C1>();
            view.each([&](const auto& c,const auto & ) { 
                total_v += c.v;
            });
        }
        std::cout << "Sum C0.v: " << total_v << std::endl;
    }
}

void bench_flecs(int entity_count, const std::vector<int>& archetypes) {
    std::cout << "--- Flecs ---" << std::endl;
    flecs::world world;
    world.defer_begin();
    {
        Timer t("Create");
        for(int i=0; i<entity_count; ++i) {
            int mask = archetypes[i % archetypes.size()];
            auto e = world.entity();
            if(mask & (1<<0)) e.add<C0>();
            if(mask & (1<<1)) e.add<C1>();
            if(mask & (1<<2)) e.add<C2>();
            if(mask & (1<<3)) e.add<C3>();
            if(mask & (1<<4)) e.add<C4>();
            if(mask & (1<<5)) e.add<C5>();
            if(mask & (1<<6)) e.add<C6>();
            if(mask & (1<<7)) e.add<C7>();
            if(mask & (1<<8)) e.add<C8>();
            if(mask & (1<<9)) e.add<C9>();
        }
    }
    world.defer_end();
    // std::cout << "Archetypes created (Flecs): " << world.count<flecs::Archetype>() << std::endl;
    {
        double total_v = 0;
        {
            Timer t("Iterate C0, C1");
            auto q = world.query<const C0,const C1>();
            q.each([&](const C0& c,const C1 & ) { 
                total_v += c.v;
            });
        }
        std::cout << "Sum C0.v: " << total_v << std::endl;
    }
}

int main() {
    const int ENTITY_COUNT = 1'000'000;
    const int ARCHETYPE_COUNT = 100;
    
    std::cout << "Benchmarking " << ENTITY_COUNT << " entities." << std::endl;

    std::vector<int> archetypes;
    std::mt19937 rng(42);
    for(int i=0; i<ARCHETYPE_COUNT; ++i) {
        int mask = rng() % 1024;
        if(mask == 0) mask = 1;
        archetypes.push_back(mask);
    }

    bench_elysia(ENTITY_COUNT, archetypes);
    std::cout << std::endl;
    bench_entt(ENTITY_COUNT, archetypes);
    std::cout << std::endl;
    bench_flecs(ENTITY_COUNT, archetypes);
    
    return 0;
}
