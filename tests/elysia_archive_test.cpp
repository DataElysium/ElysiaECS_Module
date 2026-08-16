module;
#include <gtest/gtest.h>
#include <string>

export module elysia.archive_test;

import elysia.archive;
import elysia.world;
import elysia.entity;
import elysia.meta;
import elysia.storage;

using namespace elysia;
using namespace elysia::archive;
namespace archive_test{
struct Position { float x, y; };
}
using namespace archive_test;
TEST(ElysiaArchive, BasicSaveLoad) {
    World world;
    SnapshotRegistry reg;
    reg.register_type<Position>("Position");

    world.spawn().add(Position{1.0f, 2.0f});
    world.spawn().add(Position{3.0f, 4.0f});

    // Save using Aurora (Manifest)
    auto snapshot = AuroraArchive::create(world, reg);
    EXPECT_EQ(snapshot.entities.size(), 1); 
    EXPECT_EQ(snapshot.entities[0].count, 2);

    // Load
    World world2;
    CommandBuffer cmd(&world2.index());
    auto res = archive::load_aurora(world2, cmd, reg, snapshot);
    EXPECT_TRUE(res.is_ok());
    
    world2.submit(cmd);

    EXPECT_EQ(world2.entity(Entity(0,0)).get<Position>()->x, 1.0f);
    EXPECT_EQ(world2.entity(Entity(1,0)).get<Position>()->x, 3.0f);
}

TEST(ElysiaArchive, ColumnarSaveLoad) {
    World world;
    SnapshotRegistry reg;
    reg.register_type<Position>("Position");

    world.spawn().add(Position{10.0f, 20.0f});

    // Path A: Structured Columnar (uses 'data' field)
    auto snapshot = AuroraArchive::create(world, reg, { AuroraArchive::Config::Format::Columnar });
    
    bool found_structured = false;
    for (const auto& [name, entry] : snapshot.embed) {
        // Now Columnar uses the 'data' field (Structured Path A)
        if (entry.format == "columnar" && entry.encoding == "raw" && entry.data) {
            found_structured = true;
        }
    }
    EXPECT_TRUE(found_structured);

    // Load back
    World world2;
    CommandBuffer cmd(&world2.index());
    auto res = archive::load_aurora(world2, cmd, reg, snapshot);
    EXPECT_TRUE(res.is_ok());
    world2.submit(cmd);

    EXPECT_EQ(world2.entity(Entity(0,0)).get<Position>()->x, 10.0f);
}

TEST(ElysiaArchive, RawRoundtrip) {
    World world;
    SnapshotRegistry reg;
    reg.register_type<Position>("Position");

    // Spawn 100 entities to make it a bit meaty
    for (int i = 0; i < 100; ++i) {
        world.spawn().add(Position{ (float)i, (float)i * 2.0f });
    }

    // Path B: True Binary (Raw)
    auto snapshot = AuroraArchive::create(world, reg, { AuroraArchive::Config::Format::Raw });
    
    // Dump for user to see
    auto toml = reflect::write_toml(snapshot);
    printf("--- Raw World Manifest TOML ---\\n%s\\n-------------------------------\\n", toml.c_str());

    // Load back into World 2
    World world2;
    CommandBuffer cmd(&world2.index());
    auto res = archive::load_aurora(world2, cmd, reg, snapshot);
    EXPECT_TRUE(res.is_ok());
    world2.submit(cmd);

    // Verify
    int count = 0;
    world2.query<Entity, Position>().each([&](Entity e, Position& p) {
        EXPECT_EQ(p.x, (float)e.id());
        EXPECT_EQ(p.y, (float)e.id() * 2.0f);
        count++;
    });
    EXPECT_EQ(count, 100);
}

TEST(ElysiaArchive, RawBinaryPath) {
    World world;
    SnapshotRegistry reg;
    reg.register_type<Position>("Position");

    world.spawn().add(Position{55.0f, 66.0f});

    // Path B: True Binary (uses 'blob' field)
    auto snapshot = AuroraArchive::create(world, reg, { AuroraArchive::Config::Format::Raw });
    
    bool found_binary = false;
    for (const auto& [name, entry] : snapshot.embed) {
        // Raw format uses the 'blob' field (Binary Path B)
        if (entry.format == "raw_v3" && entry.encoding == "base64" && entry.blob) {
            found_binary = true;
        }
    }
    EXPECT_TRUE(found_binary);
}

struct Velocity { float dx, dy; };

TEST(ElysiaArchive, MultiFilePatchRoundtrip) {
    World world;
    SnapshotRegistry reg;
    reg.register_type<Position>("Position");
    reg.register_type<Velocity>("Velocity");

    // 1. Prepare Data: One entity with two parts
    Entity e_orig(88, 0);
    Position pos{100.0f, 200.0f};
    Velocity vel{1.0f, 2.0f};

    // 2. Manually write physical files to simulate external sources
    // Part A: Raw Binary for Position
    std::string raw_path = "patch_part.raw";
    {
        std::vector<const ComponentFactory*> active = { &reg.factories().at(TypeTraits<Position>::id) };
        World temp_w;
        // 🌸 IMPORTANT: Must spawn at the exact same ID (88) so the binary dump matches the manifest!
        temp_w.spawn_at(Entity(88, 0));
        temp_w.entity(Entity(88, 0)).add(pos);
        
        auto* arch = static_cast<Archetype<>*>(temp_w.index().records()[88].archetype);
        auto bytes = RawTableCodec::export_archetype(*arch, active);
        
        std::ofstream ofs(raw_path, std::ios::binary);
        ofs.write(bytes.data(), bytes.size());
    }

    // Part B: CSV for Velocity
    std::string csv_path = "patch_part.csv";
    {
        std::string csv_content = "id,Velocity.dx,Velocity.dy\n88,1.0,2.0\n";
        std::ofstream ofs(csv_path);
        ofs << csv_content;
    }

    // 3. Create Manifest pointing to these files
    WorldArchive manifest;
    manifest.entities.push_back({88, 1});
    
    // Archetype 0 (Raw from file)
    ArchetypeBlob b0;
    b0.name = "pos_part";
    b0.source_uri = "file://" + raw_path;
    b0.components = {"Position"};
    b0.table_str = "1"; // Count
    manifest.archetypes.push_back(b0);

    // Archetype 1 (CSV from file)
    ArchetypeBlob b1;
    b1.name = "vel_part";
    b1.source_uri = "file://" + csv_path;
    b1.components = {"Velocity"};
    manifest.archetypes.push_back(b1);

    // 4. Load into a fresh world
    World world2;
    CommandBuffer cmd(&world2.index());
    auto res = archive::load_aurora(world2, cmd, reg, manifest);
    EXPECT_TRUE(res.is_ok());
    world2.submit(cmd);

    // 5. Verify Patching: Entity 88 should have BOTH Position and Velocity
    Entity e_new(88, 0);
    EXPECT_TRUE(world2.index().is_alive(e_new));
    
    auto* p = world2.entity(e_new).get<Position>();
    auto* v = world2.entity(e_new).get<Velocity>();
    
    ASSERT_NE(p, nullptr);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(p->x, 100.0f);
    EXPECT_EQ(v->dx, 1.0f);

    // Cleanup
    std::remove(raw_path.c_str());
    std::remove(csv_path.c_str());
}
