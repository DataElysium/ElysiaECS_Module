#include <gtest/gtest.h>
#include <atomic>
#include <vector>
#include <unordered_set>

import elysia.world;
import elysia.schedule;
import elysia.entity;
import elysia.query;
import elysia.meta;

using namespace elysia;

struct AliveFlag { bool alive = true; };

// =============================================================================
// cmd.spawn() = forward-only atomic counter (fetch_add).
// despawn puts ID into free-list (index_.free).
// world.spawn() CAN recycle despawned forward IDs from free-list.
// cmd.spawn() NEVER recycles — always forward.
// =============================================================================

// Spawn via cmd.spawn(), despawn, verify world.spawn() recycles
TEST(SchedulerSpawnDespawn, SpawnDespawnFreeListRecycle) {
    World world;
    CommandBuffer cmd(&world.index());

    Entity e[5];
    for (int i = 0; i < 5; ++i) { e[i] = cmd.spawn(); cmd.insert(e[i], AliveFlag{true}); }
    world.submit(cmd);

    uint32_t first_id = e[0].id();
    EXPECT_TRUE(world.index().is_alive(e[0]));

    CommandBuffer cmd2(&world.index());
    for (int i = 1; i <= 3; ++i) cmd2.despawn(e[i]);
    world.submit(cmd2);
    EXPECT_FALSE(world.index().is_alive(e[1]));

    Entity recycled = world.spawn().entity;
    uint32_t rid = recycled.id();
    bool among = (rid == e[1].id() || rid == e[2].id() || rid == e[3].id());
    EXPECT_TRUE(among) << "world.spawn() recycled " << rid
        << " expected {" << e[1].id() << "," << e[2].id() << "," << e[3].id() << "}";
    EXPECT_GT(world.index().records()[rid].version, 0u);

    CommandBuffer cmd3(&world.index());
    Entity fwd = cmd3.spawn();
    world.submit(cmd3);
    // Note: With the refactor, cmd.spawn() PREFERS recycled IDs.
    // So fwd.id() might be rid or another recycled ID.
    // We just verify it's a valid alive entity.
    EXPECT_TRUE(world.index().is_alive(fwd));
}

// Full cycle: concurrent spawn → exclusive despawn → recycle → concurrent spawn
TEST(SchedulerSpawnDespawn, CrossWaveSpawnDespawnRespawn) {
    World world;
    std::atomic<int> spawned_count{0};

    {
        Scheduler sched;
        for (int i = 0; i < 3; ++i) {
            sched.system("s" + std::to_string(i))
                .run([&](World*, CommandBuffer* cmd) {
                    for (int j = 0; j < 10; ++j) {
                        Entity e = cmd->spawn(); 
                        spawned_count.fetch_add(1);
                        cmd->insert(e, AliveFlag{true});
                    }
                }).build();
        }
        ForkUnionExecutor::build_from(sched)->run(&world);
    }
    EXPECT_EQ(spawned_count.load(), 30);
    int alive = 0;
    world.query<Entity, AliveFlag>().each([&](Entity, AliveFlag) { alive++; });
    EXPECT_EQ(alive, 30);

    // Collect IDs for despawn (Exclusive phase)
    std::vector<Entity> to_despawn;
    world.query<Entity, AliveFlag>().each([&](Entity e, AliveFlag) { to_despawn.push_back(e); });

    {
        Scheduler sched;
        sched.system("despawn_all")
            .run([&](World* w) { for (auto e : to_despawn) w->despawn(e); }).build();
        ForkUnionExecutor::build_from(sched)->run(&world);
    }
    alive = 0;
    world.query<Entity, AliveFlag>().each([&](Entity, AliveFlag) { alive++; });
    EXPECT_EQ(alive, 0);

    Entity recycled = world.spawn().entity;
    world.entity(recycled).add(AliveFlag{true});
    
    // The recycled ID should be one of the previously despawned ones.
    bool found = false;
    for (auto old_e : to_despawn) if (recycled.id() == old_e.id()) { found = true; break; }
    EXPECT_TRUE(found) << "world.spawn() reused " << recycled.id();

    {
        Scheduler sched;
        sched.system("new_spawn")
            .run([](World*, CommandBuffer* cmd) {
                for (int j = 0; j < 10; ++j) {
                    Entity e = cmd->spawn(); cmd->insert(e, AliveFlag{true});
                }
            }).build();
        ForkUnionExecutor::build_from(sched)->run(&world);
    }

    alive = 0;
    std::unordered_set<uint32_t> ids;
    world.query<Entity, AliveFlag>().each([&](Entity e, AliveFlag) {
        ids.insert(e.id()); alive++;
    });
    EXPECT_EQ(alive, 11);
    EXPECT_EQ(ids.size(), 11u) << "Duplicate IDs in spawn-despawn-respawn cycle";
}

struct BatchExtra { int value = 0; };

TEST(SchedulerSpawnDespawn, BatchDespawnRepairsRowsAndIgnoresDuplicates) {
    World world;
    std::vector<Entity> entities;

    for (int i = 0; i < 8; ++i) {
        auto view = world.spawn().add(AliveFlag{true});
        if ((i % 2) == 0) view.add(BatchExtra{i});
        entities.push_back(view.entity);
    }

    std::vector<Entity> to_delete = {
        entities[1], entities[2], entities[5], entities[2], Entity(9999, 0)
    };
    world.batch_despawn(to_delete);

    EXPECT_FALSE(world.index().is_alive(entities[1]));
    EXPECT_FALSE(world.index().is_alive(entities[2]));
    EXPECT_FALSE(world.index().is_alive(entities[5]));

    int alive = 0;
    world.query<Entity, AliveFlag>().each([&](Entity e, AliveFlag&) {
        EXPECT_TRUE(world.index().is_alive(e));
        alive++;
    });
    EXPECT_EQ(alive, 5);

    EXPECT_NE(world.entity(entities[0]).get<BatchExtra>(), nullptr);
    EXPECT_NE(world.entity(entities[4]).get<BatchExtra>(), nullptr);
    EXPECT_NE(world.entity(entities[6]).get<BatchExtra>(), nullptr);
}

TEST(SchedulerSpawnDespawn, CommandBufferDespawnsConsecutiveEntities) {
    World world;
    std::vector<Entity> entities;
    for (int i = 0; i < 6; ++i) {
        entities.push_back(world.spawn().add(AliveFlag{true}).entity);
    }

    CommandBuffer cmd(&world.index());
    cmd.despawn(entities[0]);
    cmd.despawn(entities[2]);
    cmd.despawn(entities[4]);
    world.submit(cmd);

    int alive = 0;
    world.query<Entity, AliveFlag>().each([&](Entity, AliveFlag&) { alive++; });
    EXPECT_EQ(alive, 3);
    EXPECT_FALSE(world.index().is_alive(entities[0]));
    EXPECT_FALSE(world.index().is_alive(entities[2]));
    EXPECT_FALSE(world.index().is_alive(entities[4]));
}

struct DropTableTag {};
struct DropTableOther {};

struct DropTableTracked {
    int* destroyed = nullptr;
    bool armed = false;

    explicit DropTableTracked(int* counter) : destroyed(counter), armed(true) {}
    DropTableTracked(const DropTableTracked&) = delete;
    DropTableTracked& operator=(const DropTableTracked&) = delete;
    DropTableTracked(DropTableTracked&& other) noexcept
        : destroyed(other.destroyed), armed(other.armed) {
        other.armed = false;
    }
    DropTableTracked& operator=(DropTableTracked&& other) noexcept {
        if (this != &other) {
            destroyed = other.destroyed;
            armed = other.armed;
            other.armed = false;
        }
        return *this;
    }
    ~DropTableTracked() {
        if (armed && destroyed) ++(*destroyed);
    }
};

TEST(SchedulerSpawnDespawn, DropArchetypesWithComponentClearsWholeTables) {
    World world;
    int destroyed = 0;

    auto e1 = world.spawn().add(AliveFlag{true}).add(DropTableTag{}).entity;
    auto e2 = world.spawn().add(AliveFlag{true}).add(DropTableTag{}).add(BatchExtra{7}).entity;
    auto e3 = world.spawn().add(AliveFlag{true}).entity;
    auto e4 = world.spawn().add(AliveFlag{true}).add(DropTableOther{}).entity;
    auto e5 = world.spawn().add(AliveFlag{true}).add(DropTableTag{}).add(DropTableTracked{&destroyed}).entity;

    size_t removed = world.drop_archetypes_with<DropTableTag>();

    EXPECT_EQ(removed, 3u);
    EXPECT_EQ(destroyed, 1);
    EXPECT_FALSE(world.index().is_alive(e1));
    EXPECT_FALSE(world.index().is_alive(e2));
    EXPECT_FALSE(world.index().is_alive(e5));
    EXPECT_TRUE(world.index().is_alive(e3));
    EXPECT_TRUE(world.index().is_alive(e4));

    int tagged = 0;
    world.query<Entity>().filter<With<DropTableTag>>().each([&](Entity) { tagged++; });
    EXPECT_EQ(tagged, 0);

    int alive = 0;
    world.query<Entity, AliveFlag>().each([&](Entity e, AliveFlag&) {
        EXPECT_TRUE(world.index().is_alive(e));
        alive++;
    });
    EXPECT_EQ(alive, 2);

    auto e6 = world.spawn().add(AliveFlag{true}).add(DropTableTag{}).entity;
    EXPECT_TRUE(world.index().is_alive(e6));

    tagged = 0;
    world.query<Entity>().filter<With<DropTableTag>>().each([&](Entity) { tagged++; });
    EXPECT_EQ(tagged, 1);
}
