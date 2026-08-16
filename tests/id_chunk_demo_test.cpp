// =============================================================================
// Chunk 耗尽问题演示
//
// 场景: chunk size = 8, 但系统想 spawn 12 个实体
// chunk 在并行阶段用完后怎么办?
//
// 当前方案约束: 并行阶段不能碰 EntityIndex (线程不安全)
// chunk 用完 = 无法继续 spawn
//
// 解法 (三选一):
//   A. 增大 chunk size (简单粗暴)
//   B. Per-system 统计上帧 spawn 量, 动态调整 chunk
//   C. 耗尽 → defer: 剩余的 spawn 推迟到 flush
// =============================================================================

#include <gtest/gtest.h>
#include <vector>

import elysia.world;
import elysia.entity;

using namespace elysia;

struct Mark { int val; };

// 测试 1: chunk 刚好够用 — 正常场景
TEST(IdChunkExhaustion, ChunkExactlyEnough) {
    const int CHUNK = 4;
    World world;

    // init: 预分配 chunk
    std::vector<Entity> chunk;
    for (int i = 0; i < CHUNK; ++i)
        chunk.push_back(world.index().spawn());
    size_t cursor = 0;

    // 并行: spawn 4 个, chunk 刚好用完
    CommandBuffer cmd(&world.index());
    for (int i = 0; i < 4; ++i) {
        ASSERT_LT(cursor, chunk.size()) << "chunk exhausted at i=" << i;
        Entity e = chunk[cursor++];
        cmd.spawn(e);
        cmd.insert(e, Mark{i});
    }
    EXPECT_EQ(cursor, CHUNK);

    // flush
    world.submit(cmd);
    int count = 0;
    world.query<Entity, Mark>().each([&](Entity, Mark) { count++; });
    EXPECT_EQ(count, 4);
}

// 测试 2: chunk 不够用, cursor >= size → 无法继续
TEST(IdChunkExhaustion, ChunkExhausted_OverflowDetected) {
    const int CHUNK = 3;
    const int WANT_SPAWN = 10;

    World world;
    std::vector<Entity> chunk;
    for (int i = 0; i < CHUNK; ++i)
        chunk.push_back(world.index().spawn());
    size_t cursor = 0;

    CommandBuffer cmd(&world.index());
    int spawned = 0;

    for (int i = 0; i < WANT_SPAWN; ++i) {
        if (cursor >= chunk.size()) break;  // 耗尽 — 停止
        Entity e = chunk[cursor++];
        cmd.spawn(e);
        cmd.insert(e, Mark{i});
        spawned++;
    }

    EXPECT_EQ(spawned, CHUNK) << "must stop at chunk boundary";
    EXPECT_GT(WANT_SPAWN, spawned) << "remaining spawns would need defer or retry";

    world.submit(cmd);
    int count = 0;
    world.query<Entity, Mark>().each([&](Entity, Mark) { count++; });
    EXPECT_EQ(count, CHUNK);
}
