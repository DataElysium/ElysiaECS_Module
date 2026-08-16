 
#include <vector>
#include <chrono>
#include <iostream>
#include <string>
#include <random>
#include <iomanip>
#include <cstring>
#include <unordered_map>
 
import elysia.archive;
import elysia.world;
import elysia.reflect_wrapper;
import elysia.entity;
import elysia.meta;
import elysia.result;
import elysia.storage;

using namespace elysia;
using namespace elysia::archive;

struct BenchData {
    float x, y, z;
    float r, g, b, a;
    int id;
};

// Timer helper
struct Timer {
    std::string name;
    std::chrono::high_resolution_clock::time_point start;
    Timer(std::string n) : name(n), start(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        double ms = us / 1000.0;
        std::cout << "[" << std::left << std::setw(30) << name << "] " 
                  << std::right << std::setw(10) << us << " us "
                  << "(" << std::fixed << std::setprecision(2) << ms << " ms)" << std::endl;
    }
};

int main() {
    constexpr int COUNT = 100'000;
    std::cout << "=== Elysia Archive FINAL Benchmark (" << COUNT << " entities) ===" << std::endl;
    
    World world;
    SnapshotRegistry reg;
    reg.register_type<BenchData>("BenchData");

    for(int i=0; i<COUNT; ++i) {
        world.spawn().add(BenchData{1.0f*i, 2.0f*i, 3.0f*i, 1.0f, 0.5f, 0.0f, 1.0f, i});
    }

    // --- 1. RawArchive (The Speed King) ---
    std::vector<char> raw_data;
    {
        Timer t("RawArchive::pack");
        auto res = RawArchive::pack(world);
        if (res.is_err()) {
            std::cout << "  !! Pack Error: " << res.error().message << std::endl;
            return 1;
        }
        raw_data = std::move(res.unwrap());
    }
    std::cout << "  -> Raw Size: " << raw_data.size() / 1024 << " KB" << std::endl;

    {
        World world2;
        Timer t("RawArchive::unpack");
        auto res = RawArchive::unpack<BenchData>(world2, raw_data);
        if(res.is_err()) std::cout << "  !! Unpack Error: " << res.error().message << std::endl;
    }

    // --- 2. MsgPackArchive (The Balanced Warrior) ---
    std::vector<char> msgpack_data;
    {
        Timer t("MsgPackArchive::create");
        msgpack_data = MsgPackArchive::create(world, reg);
    }
    std::cout << "  -> MsgPack Size: " << msgpack_data.size() / 1024 << " KB" << std::endl;

    {
        World world2; CommandBuffer cmd;
        Timer t("MsgPackArchive::load");
        MsgPackArchive::load(world2, cmd, reg, msgpack_data);
        world2.submit(cmd);
    }

    // --- 3. AuroraArchive (Columnar Embed) ---
    WorldArchive aurora_col;
    {
        Timer t("Aurora(Col)::create");
        aurora_col = AuroraArchive::create(world, reg, AuroraArchive::Config{AuroraArchive::Config::Format::Columnar});
    }

    {
        World world2; CommandBuffer cmd(&world2.index());
        Timer t("Aurora(Col)::load");
        load_aurora(world2, cmd, reg, aurora_col);
        world2.submit(cmd);
    }

    // --- 4. AuroraArchive (CSV Embed) ---
    WorldArchive aurora_csv;
    {
        Timer t("Aurora(CSV)::create");
        aurora_csv = AuroraArchive::create(world, reg, AuroraArchive::Config{AuroraArchive::Config::Format::Csv});
    }

    {
        World world2; CommandBuffer cmd;
        Timer t("Aurora(CSV)::load");
        load_aurora(world2, cmd, reg, aurora_csv);
        world2.submit(cmd);
    }

    std::cout << "=== All Systems Go! Pass the chicken skewers! ===" << std::endl;

    return 0;
}
