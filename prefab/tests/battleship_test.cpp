#include <gtest/gtest.h>
#include "../examples/battleship_model.hpp"

namespace {
using namespace battleship_demo;
TEST(BattleshipPrefab, JsonTreeClonesWithIndependentReferencesAndParameters) {
    auto prefab = prepare(read_file("fixtures/battleship.json"));
    World world;
    for (int i = 0; i < 30; ++i) world.spawn(); // Template and instance IDs must differ.
    const std::array<SpawnParams, 2> parameters{{{"A", {100, 0, 0}, 1}, {"B", {200, 0, 0}, 2}}};
    auto fleet = p::spawn_native_batch(world, prefab, std::span<const SpawnParams>(parameters));
    ASSERT_EQ(fleet.size(), 2u);
    for (size_t i = 0; i < fleet.size(); ++i) {
        ASSERT_NO_THROW(verify(world, fleet[i]));
        auto ids = records(world, fleet[i]);
        EXPECT_EQ(world.get_component<Identity>(fleet[i].root())->callsign, parameters[i].callsign);
        EXPECT_EQ(world.get_component<Identity>(fleet[i].root())->faction, parameters[i].faction);
        EXPECT_EQ(world.get_component<Transform>(fleet[i].root())->position, parameters[i].position);
        for (uint32_t gun : {11u, 21u, 31u}) {
            EXPECT_EQ(world.get_component<Gun>(ids.at(gun))->caliber_mm, 410);
            EXPECT_EQ(world.get_component<Gun>(ids.at(gun))->barrels, 3);
        }
    }
    world.get_component<Hull>(fleet[0].root())->hitpoints = 0;
    auto first = records(world, fleet[0]);
    elysia::despawn_subtree(world, first.at(10)); // Destroy mount A and its battery.
    EXPECT_EQ(elysia::collect_subtree(world, fleet[0].root()).size(), 9u);
    EXPECT_EQ(world.get_component<Hull>(fleet[1].root())->hitpoints, 1800);
    EXPECT_NO_THROW(verify(world, fleet[1]));
    World another_world;
    auto fresh = p::spawn_native(another_world, prefab, parameters[0]);
    EXPECT_NO_THROW(verify(another_world, fresh));
    EXPECT_EQ(another_world.get_component<Hull>(fresh.root())->hitpoints, 1800);
}
TEST(BattleshipPrefab, AuthoredValuesSurviveNativePreparation) {
    auto json = read_file("fixtures/battleship.json");
    auto position = json.find("410.0");
    ASSERT_NE(position, std::string::npos);
    json.replace(position, 5, "406.0");
    auto prefab = prepare(json);
    World world;
    auto ship = p::spawn_native(world, prefab, SpawnParams{"Modified", {0, 0, 0}, 1});
    auto ids = records(world, ship);
    for (uint32_t gun : {11u, 21u, 31u}) EXPECT_EQ(world.get_component<Gun>(ids.at(gun))->caliber_mm, 406);
}
TEST(BattleshipPrefab, UnresolvedFileReferenceRejectsPreparation) {
    auto json = read_file("fixtures/battleship.json");
    auto position = json.find("$id:50");
    ASSERT_NE(position, std::string::npos);
    json.replace(position, 6, "$id:999");
    EXPECT_THROW(prepare(json), p::Error);
}
TEST(BattleshipPrefab, MissingNativeParameterTargetRejectsPreparation) {
    auto json = read_file("fixtures/battleship.json");
    const std::string identity = "\"Identity\": {\"callsign\": \"template\", \"faction\": 0},";
    auto position = json.find(identity);
    ASSERT_NE(position, std::string::npos);
    json.erase(position, identity.size());
    EXPECT_THROW(prepare(json), p::Error);
}
}
