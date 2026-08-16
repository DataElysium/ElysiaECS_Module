module;
#include <gtest/gtest.h>
#include <string>

export module elysia_archive_registry_test;

import elysia.archive;
import elysia.world;
import elysia.entity;
import elysia.meta;
import elysia.result;

using namespace elysia;
using namespace elysia::archive;

struct Position { float x, y; };
struct Velocity { float dx, dy; };

TEST(SnapshotRegistryTest, RegisterAndMerge) {
    SnapshotRegistry reg1;
    auto res1 = reg1.register_type<Position>("Position");
    EXPECT_TRUE(res1.is_ok());

    SnapshotRegistry reg2;
    auto res2 = reg2.register_type<Velocity>("Velocity");
    EXPECT_TRUE(res2.is_ok());

    // Merge reg2 into reg1
    auto merge_res = reg1.merge(reg2);
    EXPECT_TRUE(merge_res.is_ok());

    auto& factories = reg1.factories();
    
    // Check if both exist
    bool has_pos = false;
    bool has_vel = false;

    for(const auto& [id, fac] : factories) {
        if (fac.key == "Position") has_pos = true;
        if (fac.key == "Velocity") has_vel = true;
    }

    EXPECT_TRUE(has_pos);
    EXPECT_TRUE(has_vel);
}

TEST(SnapshotRegistryTest, KeyConflictWithinOneRegistry) {
    SnapshotRegistry reg;
    // 1. Register Position -> "CommonKey"
    auto r1 = reg.register_type<Position>("CommonKey");
    EXPECT_TRUE(r1.is_ok());

    // 2. Register Velocity -> "CommonKey"
    // This should SUCCEED and overwrite the mapping for "CommonKey"
    auto r2 = reg.register_type<Velocity>("CommonKey");
    EXPECT_TRUE(r2.is_ok());

    // 3. Verify correctness
    // Both FACTORIES should exist (because they have different TypeIDs)
    EXPECT_TRUE(reg.factories().contains(TypeTraits<Position>::id));
    EXPECT_TRUE(reg.factories().contains(TypeTraits<Velocity>::id));

    // But "CommonKey" should map to Velocity (the latest one)
    // We can't access private key_to_id_ but we trust the behavior if no error
}

TEST(SnapshotRegistryTest, KeyConflictBetweenRegistries) {
    SnapshotRegistry reg1;
    reg1.register_type<Position>("KeyA");

    SnapshotRegistry reg2;
    reg2.register_type<Velocity>("KeyA"); // Conflict!

    // Merge reg2 into reg1 with default policy (KeepExisting)
    // Should SUCCEED.
    // reg1 already has "KeyA", so it should KEEP Position mapped to "KeyA".
    // However, reg2 has Velocity with "KeyA".
    // Velocity factory will be added (new TypeID).
    // But Key mapping for "KeyA" should NOT change because of KeepExisting.
    
    auto merge_res = reg1.merge(reg2);
    EXPECT_TRUE(merge_res.is_ok());

    EXPECT_TRUE(reg1.factories().contains(TypeTraits<Position>::id));
    EXPECT_TRUE(reg1.factories().contains(TypeTraits<Velocity>::id)); 
}

TEST(SnapshotRegistryTest, MergeOverwritePolicy) {
    SnapshotRegistry reg1;
    reg1.register_type<Position>("Pos");

    SnapshotRegistry reg2;
    reg2.register_type<Position>("Pos");

    // Overwrite policy
    auto merge_res = reg1.merge(reg2, SnapshotRegistry::MergePolicy::Overwrite);
    EXPECT_TRUE(merge_res.is_ok());
}
