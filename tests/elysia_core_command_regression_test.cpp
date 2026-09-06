#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory_resource>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#if defined(__unix__)
#include <unistd.h>
#endif

import elysia.world;
import elysia.core;
import elysia.entity;
import elysia.meta;
import elysia.table;
import elysia.storage;
import elysia.config;
import elysia.observer;

namespace {
using namespace elysia;

struct RegressionPosition {
    int value;
};

struct OwnedValue {
    std::unique_ptr<int> value;
};

// Counts actual object lifetimes without leaking heap allocations when the
// command-buffer destructor under test fails to destroy its payloads.
struct LifetimeProbe {
    int* live;
    explicit LifetimeProbe(int& count) : live(&count) { ++*live; }
    LifetimeProbe(const LifetimeProbe& other) : live(other.live) { ++*live; }
    LifetimeProbe(LifetimeProbe&& other) noexcept : live(other.live) { ++*live; }
    ~LifetimeProbe() { --*live; }
};

struct alignas(64) AlignedComponent {
    std::byte bytes[64];
};

// A conforming resource that deliberately provides no accidental 64-byte
// alignment for requests below 64. This makes alignment failures deterministic.
class MinimumAlignmentResource final : public std::pmr::memory_resource {
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto* base = static_cast<std::byte*>(
            std::pmr::new_delete_resource()->allocate(bytes + 64, std::max(size_t{64}, alignment)));
        return alignment < 64 ? base + alignment : base;
    }

    void do_deallocate(void* ptr, size_t bytes, size_t alignment) override {
        auto* base = static_cast<std::byte*>(ptr);
        if (alignment < 64) base -= alignment;
        std::pmr::new_delete_resource()->deallocate(
            base, bytes + 64, std::max(size_t{64}, alignment));
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

// Finding 1: success is required. A crash or timeout fails only this test,
// rather than terminating the remaining regression tests.
TEST(ElysiaCoreCommandRegressionDeathTest, PlainDeferredSpawnCompletes) {
#if GTEST_HAS_DEATH_TEST
    ASSERT_EXIT({
#if defined(__unix__)
        alarm(5);
#endif
        int result = 0;
        {
            World world;
            CommandBuffer cmd(&world.index());
            const Entity e = cmd.spawn();
            world.submit(cmd);
            if (!world.index().is_alive(e) || !cmd.headers().empty()) result = 1;
        }
        std::_Exit(result);
    }, ::testing::ExitedWithCode(0), "");
#else
    GTEST_SKIP() << "Requires subprocess support to isolate the suspected invalid iterator access";
#endif
}

// Finding 2a: explicit bundled IDs must not be allocated again.
TEST(ElysiaCoreCommandRegression, ExplicitFusedSpawnClaimsId) {
    World world;
    const Entity imported(7, 0);
    CommandBuffer cmd(&world.index());
    cmd.spawn(imported);
    cmd.insert(imported, RegressionPosition{42});
    world.submit(cmd);
    ASSERT_TRUE(world.index().is_alive(imported));

    for (int i = 0; i < 8; ++i) {
        const Entity fresh = world.spawn().entity;
        EXPECT_NE(fresh.id(), imported.id()) << "Allocation reused an imported live ID";
    }
    auto* position = world.entity(imported).get<RegressionPosition>();
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->value, 42);
}

// Finding 2b: a conflicting generation must not replace a live entity.
TEST(ElysiaCoreCommandRegression, FusedSpawnRejectsConflictingGeneration) {
    World world;
    const Entity original = world.spawn().add(RegressionPosition{42}).entity;
    const Entity conflict(original.id(), static_cast<uint16_t>(original.version() + 1));
    CommandBuffer cmd(&world.index());
    cmd.spawn(conflict);
    cmd.insert(conflict, RegressionPosition{99});
    world.submit(cmd);

    EXPECT_TRUE(world.index().is_alive(original));
    EXPECT_FALSE(world.index().is_alive(conflict));
    auto* position = world.entity(original).get<RegressionPosition>();
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->value, 42);
}

// Finding 2c: explicitly claiming a recycled ID must remove it from the pool.
TEST(ElysiaCoreCommandRegression, ExplicitFusedSpawnClaimsRecycledId) {
    World world;
    const Entity original = world.spawn().entity;
    // Settle reservations before freeing the entity to isolate this check
    // from finding 8 (cleanup discarding newly freed IDs).
    CommandBuffer cmd(&world.index());
    world.submit(cmd);
    world.despawn(original);
    ASSERT_EQ(world.index().recycled_ids().size(), 1u);

    const Entity restored(original.id(), static_cast<uint16_t>(original.version() + 1));
    cmd.spawn(restored);
    cmd.insert(restored, RegressionPosition{42});
    world.submit(cmd);

    ASSERT_TRUE(world.index().is_alive(restored));
    ASSERT_TRUE(world.index().recycled_ids().empty());
    EXPECT_NE(world.spawn().entity.id(), restored.id());
    auto* position = world.entity(restored).get<RegressionPosition>();
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->value, 42);
}

// Finding 3: updating a bundle in its current archetype must retain one row.
TEST(ElysiaCoreCommandRegression, SameArchetypeFusedSpawnDoesNotDuplicateRows) {
    World world;
    const Entity e = world.spawn().add(RegressionPosition{1}).entity;
    CommandBuffer cmd(&world.index());
    cmd.spawn(e);
    cmd.insert(e, RegressionPosition{2});
    world.submit(cmd);

    int count = 0;
    world.query<Entity, const RegressionPosition>().each(
        [&](Entity found, const RegressionPosition& p) {
            EXPECT_EQ(found, e);
            EXPECT_EQ(p.value, 2);
            ++count;
        });
    EXPECT_EQ(count, 1);

    world.despawn(e);
    count = 0;
    world.query<Entity, const RegressionPosition>().each(
        [&](Entity, const RegressionPosition&) { ++count; });
    EXPECT_EQ(count, 0) << "Despawn left a ghost row";
}

// Finding 4a: fusion must preserve the last-write-wins behavior of inserts.
// Pre-create the archetype to isolate payload selection from duplicate columns.
TEST(ElysiaCoreCommandRegression, RepeatedFusedInsertKeepsLastValue) {
    World world;
    world.spawn().add(RegressionPosition{0});
    CommandBuffer cmd(&world.index());
    const Entity e = cmd.spawn();
    cmd.insert(e, RegressionPosition{1});
    cmd.insert(e, RegressionPosition{2});
    world.submit(cmd);

    auto* position = world.entity(e).get<RegressionPosition>();
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->value, 2);
}

// Finding 4b: first materialization must not create duplicate columns.
TEST(ElysiaCoreCommandRegression, RepeatedFusedInsertCreatesOneColumn) {
    World world;
    CommandBuffer cmd(&world.index());
    const Entity e = cmd.spawn();
    cmd.insert(e, RegressionPosition{1});
    cmd.insert(e, RegressionPosition{2});
    world.submit(cmd);

    auto record = world.index().lookup(e);
    ASSERT_TRUE(record.is_ok());
    auto* arch = static_cast<Archetype<DefaultConfig>*>(record.unwrap()->archetype);
    ASSERT_NE(arch, nullptr);
    EXPECT_EQ(arch->types().size(), 1u);
}

// Finding 5: the null sentinel must be distinct from all allocated entities.
TEST(ElysiaCoreCommandRegression, FirstEntityIsValidAndNotNull) {
    World world;
    const Entity first = world.spawn().entity;
    EXPECT_TRUE(first.is_valid());
    EXPECT_NE(first, Entity{});
    EXPECT_FALSE(world.index().is_alive(Entity{}));
    EXPECT_TRUE(world.index().lookup(Entity{}).is_err());
}

TEST(ElysiaCoreCommandRegression, DespawningNullPreservesFirstEntity) {
    World world;
    const Entity first = world.spawn().entity;
    world.despawn(Entity{});
    EXPECT_TRUE(world.index().is_alive(first));
}

// Finding 6: inspect raw slots before constructing an over-aligned object, so
// the test itself never dereferences a misaligned AlignedComponent pointer.
TEST(ElysiaCoreCommandRegression, ChunkAllocationHonorsComponentAlignment) {
    MinimumAlignmentResource resource;
    const std::array<const TypeInfo*, 1> types{get_type_info_ptr<AlignedComponent>()};
    Chunk chunk(2, types, &resource);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(chunk.component(0, 0)) % alignof(AlignedComponent), 0u);
}

TEST(ElysiaCoreCommandRegression, ChunkResizeHonorsComponentAlignment) {
    MinimumAlignmentResource resource;
    const std::array<const TypeInfo*, 1> types{get_type_info_ptr<AlignedComponent>()};
    Chunk chunk(2, types, &resource);
    chunk.resize(4);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(chunk.component(0, 0)) % alignof(AlignedComponent), 0u);
}

// Finding 7: abandoning a buffer must destroy components and captures.
TEST(ElysiaCoreCommandRegression, AbandonedBufferDestroysComponentPayload) {
    int live = 0;
    {
        CommandBuffer cmd;
        cmd.insert(Entity(7, 0), LifetimeProbe{live});
        ASSERT_EQ(live, 1);
    }
    EXPECT_EQ(live, 0);
}

TEST(ElysiaCoreCommandRegression, AbandonedBufferDestroysCallbackCapture) {
    int live = 0;
    {
        CommandBuffer cmd;
        cmd.call([probe = LifetimeProbe{live}](void*) { (void)probe; });
        ASSERT_EQ(live, 1);
    }
    EXPECT_EQ(live, 0);
}

// Positive control for the lifetime probe and the existing explicit cleanup.
TEST(ElysiaCoreCommandRegression, ExplicitClearDestroysPayloadsExactlyOnce) {
    int live = 0;
    {
        CommandBuffer cmd;
        cmd.insert(Entity(7, 0), LifetimeProbe{live});
        cmd.call([probe = LifetimeProbe{live}](void*) { (void)probe; });
        ASSERT_EQ(live, 2);
        cmd.clear();
        EXPECT_EQ(live, 0);
        cmd.clear();
        EXPECT_EQ(live, 0);
    }
    EXPECT_EQ(live, 0);
}

// Finding 8: failed recycle attempts must not consume subsequently freed IDs.
TEST(ElysiaCoreCommandRegression, SubmissionPreservesNewlyFreedIdsForReuse) {
    World world;
    std::vector<uint32_t> freed;
    CommandBuffer cmd(&world.index());
    for (int i = 0; i < 4; ++i) {
        const Entity e = world.spawn().entity;
        freed.push_back(e.id());
        cmd.despawn(e);
    }
    world.submit(cmd);
    EXPECT_EQ(world.index().recycled_ids().size(), freed.size());

    for (int i = 0; i < 4; ++i) {
        const Entity reused = world.spawn().entity;
        auto it = std::find(freed.begin(), freed.end(), reused.id());
        ASSERT_NE(it, freed.end()) << "Allocated a new ID while freed IDs should be available";
        EXPECT_GT(reused.version(), 0);
        freed.erase(it);
    }
    EXPECT_TRUE(freed.empty());
}
TEST(ElysiaCoreCommandRegression, SameArchetypeReplacementDestroysOldPayload) {
    int live = 0;
    {
        World world;
        const Entity e = world.spawn().add(LifetimeProbe{live}).entity;
        CommandBuffer cmd(&world.index());
        cmd.spawn(e);
        cmd.insert(e, LifetimeProbe{live});
        ASSERT_EQ(live, 2);
        world.submit(cmd);
        EXPECT_EQ(live, 1);
        world.despawn(e);
        EXPECT_EQ(live, 0);
    }
    EXPECT_EQ(live, 0);
}

TEST(ElysiaCoreCommandRegression, RepeatedOwningInsertPreservesObserverOrder) {
    World world;
    std::vector<int> observed;
    world.observer().on_add<OwnedValue>([&](Entity e) {
        auto* value = world.entity(e).get<OwnedValue>();
        ASSERT_NE(value, nullptr);
        ASSERT_NE(value->value, nullptr);
        observed.push_back(*value->value);
    });
    CommandBuffer cmd(&world.index());
    const Entity e = cmd.spawn();
    cmd.insert(e, OwnedValue{std::make_unique<int>(1)});
    cmd.insert(e, OwnedValue{std::make_unique<int>(2)});
    world.submit(cmd);
    EXPECT_EQ(observed, (std::vector<int>{1, 2}));
    auto* value = world.entity(e).get<OwnedValue>();
    ASSERT_NE(value, nullptr);
    ASSERT_NE(value->value, nullptr);
    EXPECT_EQ(*value->value, 2);
}

TEST(ElysiaCoreCommandRegression, NullCannotBeExplicitlySpawned) {
    World world;
    const auto capacity = world.index().records().size();
    EXPECT_TRUE(world.spawn_at(Entity{}).is_err());
    CommandBuffer cmd(&world.index());
    cmd.spawn(Entity{});
    cmd.insert(Entity{}, RegressionPosition{42});
    world.submit(cmd);
    EXPECT_FALSE(world.index().is_alive(Entity{}));
    EXPECT_EQ(world.index().records().size(), capacity);
    int count = 0;
    world.query<Entity>().each([&](Entity) { ++count; });
    EXPECT_EQ(count, 0);
}

TEST(ElysiaCoreCommandRegression, MovingBufferTransfersPayloadOwnership) {
    int live = 0;
    {
        CommandBuffer source;
        source.insert(Entity(7, 0), LifetimeProbe{live});
        CommandBuffer destination(std::move(source));
        source.clear();
        EXPECT_EQ(live, 1);
        destination.clear();
        EXPECT_EQ(live, 0);
    }
    EXPECT_EQ(live, 0);
}

TEST(ElysiaCoreCommandRegression, MoveAssignmentDestroysReplacedPayloads) {
    int source_live = 0, destination_live = 0;
    {
        CommandBuffer source;
        source.call([probe = LifetimeProbe{source_live}](void*) { (void)probe; });
        CommandBuffer destination;
        destination.insert(Entity(7, 0), LifetimeProbe{destination_live});
        destination = std::move(source);
        source.clear();
        EXPECT_EQ(source_live, 1);
        EXPECT_EQ(destination_live, 0);
    }
    EXPECT_EQ(source_live, 0);
    EXPECT_EQ(destination_live, 0);
}

TEST(ElysiaCoreCommandRegression, ExhaustedBatchDoesNotConsumeLaterFreeId) {
    EntityIndex index;
    const Entity first = index.spawn();
    const Entity second = index.spawn();
    const Entity later = index.spawn();
    index.free(first);
    index.free(second);
    const auto batch = index.reserve_batch(4);
    ASSERT_EQ(batch.recycled_ids.size(), 2u);
    EXPECT_EQ(batch.new_count, 2u);
    // No batch span is used after free_id may reallocate the pool.
    index.free_id(later.id());
    index.cleanup_recycled_pool();
    ASSERT_EQ(index.recycled_ids().size(), 1u);
    EXPECT_EQ(index.reserve_id(), later.id());
}

TEST(ElysiaCoreCommandRegression, ParallelReservationsRemainUniqueAcrossRecycling) {
    EntityIndex index;
    for (int i = 0; i < 16; ++i) {
        const Entity e = index.spawn();
        index.free(e);
    }
    index.cleanup_recycled_pool();
    std::array<std::array<uint32_t, 64>, 4> ids{};
    std::vector<std::thread> workers;
    for (auto& block : ids) {
        workers.emplace_back([&index, &block] {
            for (auto& id : block) id = index.reserve_id();
        });
    }
    for (auto& worker : workers) worker.join();
    std::vector<uint32_t> all;
    for (const auto& block : ids) all.insert(all.end(), block.begin(), block.end());
    std::sort(all.begin(), all.end());
    EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
}
} // namespace
