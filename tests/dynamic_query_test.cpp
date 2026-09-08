#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <vector>
import elysia;

namespace {
using namespace elysia;
constexpr uint64_t payload_id = fnv1a_64("plugin::Payload");
constexpr uint64_t tag_id = fnv1a_64("plugin::Selected");
constexpr uint64_t excluded_id = fnv1a_64("plugin::Excluded");

Entity opaque(World& world, const TypeInfo* type, double value) {
    alignas(64) std::array<std::byte, 64> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    auto entity = world.spawn().entity;
    world.add_component_dynamic(entity, type, bytes.data());
    return entity;
}
size_t count(DynamicQuery& query) {
    size_t result = 0;
    query.each_chunk([&](const DynamicChunkView& chunk) { result += chunk.entities.size(); });
    return result;
}

TEST(DynamicQuery, OpaqueColumnsSupportRuntimeReadWriteAndChunkAlignment) {
    World world;
    auto* info = world.graph().registry().register_opaque(payload_id, "plugin::Payload", 64, 64);
    for (int i = 0; i < 2049; ++i) opaque(world, info, i);
    DynamicQuery query({.columns = {payload_id}});
    world.update_query(query);
    size_t chunks = 0, rows = 0;
    // Exercise the erased callback API used by runtime adapters.
    struct Context { size_t* chunks; size_t* rows; World* world; } context{&chunks, &rows, &world};
    query.each_chunk([](void* raw, const DynamicChunkView& chunk) {
        auto& context = *static_cast<Context*>(raw);
        ++*context.chunks;
        ASSERT_EQ(chunk.columns.size(), 1u);
        const auto& column = chunk.columns[0];
        EXPECT_EQ(column.id, payload_id);
        EXPECT_EQ(column.stride, 64u);
        EXPECT_EQ(column.alignment, 64u);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(column.data) % 64, 0u);
        for (size_t row = 0; row < chunk.entities.size(); ++row) {
            EXPECT_TRUE(context.world->index().is_alive(chunk.entities[row]));
            auto* bytes = static_cast<std::byte*>(column.data) + row * column.stride;
            double value;
            std::memcpy(&value, bytes, sizeof(value));
            EXPECT_EQ(value, double(*context.rows));
            value += 10;
            std::memcpy(bytes, &value, sizeof(value));
            ++*context.rows;
        }
    }, &context);
    EXPECT_GT(chunks, 1u);
    EXPECT_EQ(rows, 2049u);
    double sum = 0;
    query.each_chunk([&](const DynamicChunkView& chunk) {
        for (size_t row = 0; row < chunk.entities.size(); ++row) {
            double value;
            std::memcpy(&value, static_cast<const std::byte*>(chunk.columns[0].data) + row * 64, sizeof(value));
            sum += value;
        }
    });
    EXPECT_EQ(sum, 2049.0 * 2048 / 2 + 2049 * 10);
}

TEST(DynamicQuery, RuntimeFiltersAndDisabledDefaults) {
    World world;
    auto& registry = world.graph().registry();
    auto* payload = registry.register_opaque(payload_id, "plugin::Payload", 64, 64);
    auto* tag = registry.register_opaque(tag_id, "plugin::Selected", 0, 1);
    auto* excluded = registry.register_opaque(excluded_id, "plugin::Excluded", 0, 1);
    auto active = opaque(world, payload, 1);
    auto inactive = opaque(world, payload, 2);
    auto filtered = opaque(world, payload, 3);
    opaque(world, payload, 4);
    for (auto entity : {active, inactive, filtered}) world.add_component_dynamic(entity, tag, nullptr);
    world.entity(inactive).add(DisabledTag{});
    world.add_component_dynamic(filtered, excluded, nullptr);
    DynamicQuery query({.columns = {payload_id}, .with = {tag_id}, .without = {excluded_id}});
    world.update_query(query);
    EXPECT_EQ(count(query), 1u);
    DynamicQuery all({.with = {tag_id}, .without = {excluded_id}, .include_inactive = true});
    world.update_query(all);
    EXPECT_EQ(count(all), 2u);
    DynamicQuery disabled({.with = {TypeTraits<DisabledTag>::id}});
    world.update_query(disabled);
    EXPECT_EQ(count(disabled), 1u);
    // New matching archetypes are discovered without duplicating previous matches.
    world.remove_component_dynamic(filtered, excluded_id);
    world.update_query(query);
    world.update_query(query);
    EXPECT_EQ(count(query), 2u);
    world.despawn(active);
    EXPECT_EQ(count(query), 1u);
}

TEST(DynamicQuery, SelectedColumnOrderAndWorldLocalIdsAreIndependent) {
    World first, second;
    for (auto* world : {&first, &second}) {
        auto& registry = world->graph().registry();
        if (world == &second) registry.register_opaque(tag_id, "plugin::Selected", 0, 1);
        auto* payload = registry.register_opaque(payload_id, "plugin::Payload", 64, 64);
        auto* extra = registry.register_opaque(excluded_id, "plugin::Extra", 64, 64);
        if (world == &first) registry.register_opaque(tag_id, "plugin::Selected", 0, 1);
        auto entity = opaque(*world, payload, world == &first ? 10 : 20);
        alignas(64) std::array<std::byte, 64> bytes{};
        double value = 99;
        std::memcpy(bytes.data(), &value, sizeof(value));
        world->add_component_dynamic(entity, extra, bytes.data());
    }
    DynamicQuery query({.columns = {excluded_id, payload_id}});
    for (auto* world : {&first, &second, &first}) {
        world->update_query(query);
        EXPECT_EQ(count(query), 1u);
        query.each_chunk([&](const DynamicChunkView& chunk) {
            EXPECT_EQ(chunk.columns[0].id, excluded_id);
            EXPECT_EQ(chunk.columns[1].id, payload_id);
            double value;
            std::memcpy(&value, chunk.columns[1].data, sizeof(value));
            EXPECT_EQ(value, world == &first ? 10 : 20);
        });
    }
}

TEST(DynamicQuery, InvalidDescriptorsAndUnknownIdsGiveErrors) {
    EXPECT_THROW((DynamicQuery({.columns = {payload_id, payload_id}})), std::invalid_argument);
    EXPECT_THROW((DynamicQuery({.columns = {payload_id}, .without = {payload_id}})), std::invalid_argument);
    World world;
    DynamicQuery unknown({.columns = {payload_id}});
    EXPECT_THROW(unknown.each_chunk([](const auto&) {}), std::logic_error);
    EXPECT_THROW(world.update_query(unknown), std::invalid_argument);
    auto* payload = world.graph().registry().register_opaque(payload_id, "plugin::Payload", 64, 64);
    opaque(world, payload, 1);
    world.update_query(unknown);
    EXPECT_EQ(count(unknown), 1u);
    world.graph().registry().register_opaque(tag_id, "plugin::Selected", 0, 1);
    DynamicQuery tag_column({.columns = {tag_id}});
    EXPECT_THROW(world.update_query(tag_column), std::invalid_argument);
    EXPECT_THROW(unknown.each_chunk(nullptr, nullptr), std::invalid_argument);
}

struct NativePosition { double x; };
TEST(DynamicQuery, TypedComponentsShareIdsWithRuntimeQueries) {
    World world;
    world.spawn().add(NativePosition{12});
    DynamicQuery query({.columns = {fnv1a_64(TypeTraits<NativePosition>::name())}});
    world.update_query(query);
    query.each_chunk([](const DynamicChunkView& chunk) {
        for (size_t i = 0; i < chunk.entities.size(); ++i) {
            double x = 33;
            std::memcpy(static_cast<std::byte*>(chunk.columns[0].data) + i * chunk.columns[0].stride, &x, sizeof(x));
        }
    });
    world.query<NativePosition>().each([](NativePosition& position) { EXPECT_EQ(position.x, 33); });
}
}
