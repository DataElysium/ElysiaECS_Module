#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <vector>

import elysia.world;
import elysia.schedule;
import elysia.entity;
import elysia.storage;

using namespace elysia;

struct Pos {
  float x, y;
};
struct Vel {
  float dx, dy;
};

TEST(ElysiaScheduler, HighLoadPhaseChain) {
  World world;
  const int count = 100000;

  // Spawn 100k entities
  for (int i = 0; i < count; ++i) {
    world.spawn().add(Pos{1.0f, 1.0f}).add(Vel{1.0f, 1.0f});
  }

  Scheduler scheduler;

  // 1. Configure the pipeline: 5 Phases with sync points
  scheduler.chain("First", schedule::Sync, "PreUpdate", schedule::Sync,
                  "Update", schedule::Sync, "PostUpdate", schedule::Sync,
                  "Last");

  // 2. Optimization: Manual Chunk Splitting in 'First' phase
  // We'll find the archetype and split its chunks between two parallel systems.

  // Find the archetype for (Pos, Vel)
  Archetype<> *target_arch = nullptr;
  world.graph().each([&](auto *arch) {
    if (arch->template has<Pos>() && arch->template has<Vel>()) {
      target_arch = arch;
      printf("type count: %zu\n", arch->types().size());
    }
  });
  ASSERT_NE(target_arch, nullptr);
  int aa = 0;
  size_t chunk_count = target_arch->table().chunks().size();
  size_t mid = chunk_count / 2;

  // System 1: Reset Lower Chunks
  scheduler.system("ResetLower")
      .in_set("First")
      .run([&](World *w) {
        auto col_pos = target_arch->get_column<Pos>().value();
        auto &chunks = target_arch->table().chunks();
        for (size_t i = 0; i < mid; ++i) {

          auto *chunk = chunks[i].get();
          Pos *pos_array = static_cast<Pos *>(chunk->component(col_pos, 0));
          for (size_t k = 0; k < chunk->count(); ++k) {
            pos_array[k].x = 0;
            pos_array[k].y = 0;
          }
        }
      })
      .build();

  // System 2: Reset Upper Chunks
  scheduler.system("ResetUpper")
      .in_set("First")
      .run([=, &aa](World *w) {
        auto col_pos = target_arch->get_column<Pos>().value();
        auto &chunks = target_arch->table().chunks();

        for (size_t i = mid; i < chunk_count; ++i) {
          auto *chunk = chunks[i].get();
          Pos *pos_array = static_cast<Pos *>(chunk->component(col_pos, 0));
          for (size_t k = 0; k < chunk->count(); ++k) {
            if (abs(pos_array[k].x - 1.0f) > 0.001f)
              printf("ResetUpper: %d\n", 1);
            pos_array[k].x = 0;
            pos_array[k].y = 0;
          }
        }
      })
      .build();

  scheduler.system("Integrate")
      .in_set("Update")
      .run([&](Pos &p, const Vel &v) {
        p.x += v.dx;
        p.y += v.dy;
      })
      .build();

  scheduler.system("Verify")
      .in_set("Last")
      .run([&](World *w) { printf("Verify:  %d\n", aa); })
      .build();

  // 5. Bench
  auto exec = TaskflowExecutor::build_from(scheduler);

  auto start = std::chrono::high_resolution_clock::now();
  exec->run(&world);
  auto end = std::chrono::high_resolution_clock::now();

  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  std::cout << "[Bench] Segmented Pipeline (100k entities): "
            << duration.count() << " us" << std::endl;

  // --- Serial Audit ---

  size_t total_found = 0;

  size_t in_expected_arch = 0;

  size_t value_is_zero = 0; // Reset works, but Integrate fails

  size_t value_is_one = 0; // Both Reset and Integrate failed

  size_t value_is_two = 0; // Reset failed, but Integrate worked

  world.graph().each([&](auto *arch) {
    total_found += arch->count();

    bool is_target = arch->template has<Pos>() && arch->template has<Vel>();

    if (is_target)
      in_expected_arch += arch->count();

    auto col_pos = arch->template get_column<Pos>();

    if (col_pos) {

      for (auto &chunk : arch->table().chunks()) {

        Pos *p_arr = static_cast<Pos *>(chunk->component(*col_pos, 0));

        for (size_t i = 0; i < chunk->count(); ++i) {

          if (p_arr[i].x == 0.0f)
            value_is_zero++;

          else if (p_arr[i].x == 1.0f)
            value_is_one++;

          else if (p_arr[i].x == 2.0f)
            value_is_two++;
        }
      }
    }
  });

  std::cout << "--- Pipeline Audit Report ---" << std::endl;

  std::cout << "Total Entities in World: " << total_found << std::endl;

  std::cout << "Entities in (Pos,Vel) Archetype: " << in_expected_arch
            << std::endl;

  std::cout << "Value Statistics: [0.0]: " << value_is_zero
            << " | [1.0]: " << value_is_one << " | [2.0]: " << value_is_two
            << std::endl;

  // Verification: sum x should be 100,000

  float sum_x = 0;

  world.query<Pos>().each([&](Pos &p) { sum_x += p.x; });

  EXPECT_NEAR(sum_x, (float)count, 0.1f);
}
