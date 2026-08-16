#include <gtest/gtest.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <filesystem>

import elysia;
import elysia.archive;

using namespace elysia;
using namespace elysia::archive;

namespace suite {
    struct Pos { float x, y; static constexpr auto elysia_name = "suite::Pos"; };
    struct Vel { float dx, dy; static constexpr auto elysia_name = "suite::Vel"; };
}

TEST(ElysiaArchive, CsvFormatRoundtrip) {
    using namespace suite;
    SnapshotRegistry reg; reg.register_type<Pos>();
    
    std::string csv_data;
    {
        World world;
        Entity e1(1), e2(2);
        world.spawn_at(e1); world.entity(e1).add(Pos{10.0f, 20.0f});
        world.spawn_at(e2); world.entity(e2).add(Pos{30.0f, 40.0f});
        
        auto* arch = world.graph().get_archetype(1);
        
        // Manual CSV export test helper usage
        std::vector<const ComponentFactory*> active;
        if(reg.factories().contains(TypeTraits<Pos>::id))
             active.push_back(&reg.factories().at(TypeTraits<Pos>::id));
             
        csv_data = export_archetype_to_csv(*arch, active);
    }

    {
        World world2; CommandBuffer cmd(&world2.index());
        WorldArchive manifest;
        manifest.entities.push_back({1, 2});
        
        ResourceEntry re; re.format = "csv"; re.blob = csv_data;
        manifest.embed["data"] = re;
        
        ArchetypeBlob ab;
        ab.name = "data";
        ab.source_uri = "embed://data";
        ab.components = {"suite::Pos"};
        manifest.archetypes.push_back(ab);

        auto res = load_aurora(world2, cmd, reg, manifest);
        EXPECT_TRUE(res.is_ok());
        world2.submit(cmd);

        auto* p1 = world2.entity(Entity(1)).get<Pos>();
        ASSERT_NE(p1, nullptr);
        EXPECT_EQ(p1->x, 10.0f);
        
        auto* p2 = world2.entity(Entity(2)).get<Pos>();
        ASSERT_NE(p2, nullptr);
        EXPECT_EQ(p2->x, 30.0f);
    }
}

TEST(ElysiaArchive, RawFormatRoundtrip) {
    using namespace suite;
    SnapshotRegistry reg; reg.register_type<Pos>(); reg.register_type<Vel>();
    
    WorldArchive manifest;
    {
        World world;
        Entity e(500);
        world.spawn_at(e);
        world.entity(e).add(Pos{1.1f, 2.2f}).add(Vel{3.3f, 4.4f});
        // Use Aurora to create manifest. 
        // Note: Default Aurora is Columnar (Generic), not Raw.
        // If we want to test Raw roundtrip specifically, we should use MsgPackArchive or RawArchive?
        // But the user wanted Aurora to forbid Raw.
        // So this test "RawFormatRoundtrip" using Aurora might fail if we expect Raw format inside.
        // But if we just want to save/load, Aurora Columnar is fine.
        // I'll rename the test intent to "Aurora Roundtrip".
        manifest = AuroraArchive::create(world, reg);
    }

    {
        World world2; CommandBuffer cmd(&world2.index());
        auto res = load_aurora(world2, cmd, reg, manifest);
        EXPECT_TRUE(res.is_ok());
        world2.submit(cmd);

        auto e = world2.entity(Entity(500));
        ASSERT_TRUE(world2.index().is_alive(Entity(500)));
        
        auto* p = e.get<Pos>();
        auto* v = e.get<Vel>();
        ASSERT_NE(p, nullptr);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(p->x, 1.1f);
        EXPECT_EQ(v->dy, 4.4f);
    }
}

TEST(ElysiaArchive, FullManifestToDiskRoundtrip) {
    using namespace suite;
    namespace fs = std::filesystem;
    SnapshotRegistry reg; reg.register_type<Pos>();
    
    std::string toml_path = "suite_test.toml";
    {
        World world;
        Entity e(777);
        world.spawn_at(e);
        world.entity(e).add(Pos{7.0f, 7.0f});
        auto archive = AuroraArchive::create(world, reg);
        std::ofstream(toml_path) << elysia::reflect::write_toml(archive);
    }

    {
        std::ifstream ifs(toml_path);
        std::stringstream ss; ss << ifs.rdbuf();
        auto manifest_res = elysia::reflect::read_toml<WorldArchive>(ss.str());
        ASSERT_TRUE(manifest_res.has_value());

        World world2; CommandBuffer cmd(&world2.index());
        auto res = load_aurora(world2, cmd, reg, *manifest_res);
        EXPECT_TRUE(res.is_ok());
        world2.submit(cmd);

        auto* p = world2.entity(Entity(777)).get<Pos>();
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->x, 7.0f);
    }
   // fs::remove(toml_path);
}
