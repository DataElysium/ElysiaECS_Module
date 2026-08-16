module;
#include <gtest/gtest.h>
#include <vector>
#include <unordered_map>
#include <iostream>

export module elysia_archive_remap_test;

import elysia.archive;
import elysia.world;
import elysia.entity;
import elysia.meta;

using namespace elysia;
using namespace elysia::archive;

// --- Components ---
struct Node { int val; };
struct Link { uint64_t target; };

// --- Mapper Implementation ---
class OffsetMapper : public IDMapper {
public:
    OffsetMapper(uint64_t offset) : offset_(offset) {}
    
    Entity map(uint64_t old_id) const override {
        return Entity(old_id + offset_);
    }
private:
    uint64_t offset_;
};

TEST(ElysiaArchive, RemapWithOffset) {
    // 1. Setup Source World
    World src_world;
    SnapshotRegistry reg;
    
    // Default registration (enables Generic/Raw/MsgPack)
    reg.register_type<Node>("Node");
    reg.register_type<Link>("Link");

    auto e1 = src_world.spawn().add(Node{10}).entity;
    auto e2 = src_world.spawn().add(Link{e1.id()}).entity;

    // Save using AuroraArchive (Default Columnar/MsgPack Generic Embed)
    // Note: AuroraArchive::create does NOT use Raw format by default (or at all, if strict).
    // It uses Columnar which supports Remap.
    auto snapshot = AuroraArchive::create(src_world, reg);

    // 2. Setup Dest World
    World dest_world;
    CommandBuffer cmd(&dest_world.index());
    
    for(int i=0; i<100; ++i) dest_world.spawn();
    dest_world.spawn(); // 100
    dest_world.spawn(); // 101
    
    // 3. Setup Remapping Logic
    uint64_t offset = 100;
    OffsetMapper mapper(offset);
    
    IDRemapRegistry id_reg;
    id_reg.register_remap_hook<Link>([](Link& l, const IDMapper& m) {
        l.target = m.map(l.target).id();
    });

    // 4. Load with Remap
    auto res = archive::load_aurora_with_remap(dest_world, cmd, reg, id_reg, mapper, snapshot);
    if (res.is_err()) std::cout << "Load Error: " << res.error().message << std::endl;
    EXPECT_TRUE(res.is_ok());
    
    dest_world.submit(cmd);

    // 5. Verify
    Entity new_e1(0 + offset);
    Entity new_e2(1 + offset);

    auto* n = dest_world.entity(new_e1).get<Node>();
    ASSERT_TRUE(n != nullptr);
    EXPECT_EQ(n->val, 10);

    auto* l = dest_world.entity(new_e2).get<Link>();
    ASSERT_TRUE(l != nullptr);
    EXPECT_EQ(l->target, 100);
}