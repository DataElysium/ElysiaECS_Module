#include <gtest/gtest.h>
#include <string>
#include <limits>
#include <vector>

import elysia;
import elysia.archive;

using namespace elysia;
using namespace elysia::archive;

namespace proxy_test {
    // 🌸 1. The "Beast" (Non-serializable runtime component)
    struct TextureAsset {
        uint32_t handle; // Imagine this is a GL handle
        std::string internal_name;
        
        static constexpr auto elysia_name = "proxy_test::TextureAsset";
    };

    // 🌸 2. The "Beauty" (Clean serialization proxy)
    struct TextureProxy {
        std::string asset_path;

        // Requirement: from(T) -> T1
        static TextureProxy from(const TextureAsset& t) {
            return { "assets/" + t.internal_name + ".png" };
        }

        // Requirement: into() -> T
        TextureAsset into() const {
            // Mocking a resource load
            uint32_t new_handle = (asset_path == "assets/grass.png") ? 42 : 0;
            std::string name = asset_path.substr(7, asset_path.size() - 7 - 4);
            return { new_handle, name };
        }
    };
}

TEST(ElysiaArchive, SerializationProxyRoundtrip) {
    using namespace proxy_test;
    
    SnapshotRegistry reg;
    // 🌸 Register with proxy!
    reg.register_type_with_proxy<TextureAsset, TextureProxy>();

    std::vector<char> buffer;
    {
        World world;
        world.spawn().add(TextureAsset{ 123, "grass" });
        
        // Save using MsgPack (it uses the proxy MC codec)
        buffer = MsgPackArchive::create(world, reg);
        ASSERT_GT(buffer.size(), 0);
    }

    {
        World world2;
        CommandBuffer cmd(&world2.index());
        
        // Load using MsgPack (it uses the proxy decode logic)
        auto res = MsgPackArchive::load(world2, cmd, reg, buffer);
        ASSERT_TRUE(res.is_ok()) << res.error().message;
        world2.submit(cmd);

        auto q = world2.query<TextureAsset>();
        int count = 0;
        q.each([&](TextureAsset& t) {
            EXPECT_EQ(t.handle, 42); // Logic from proxy::into()
            EXPECT_EQ(t.internal_name, "grass");
            count++;
        });
        EXPECT_EQ(count, 1);
    }
}

TEST(ElysiaArchive, ProxyGenericCodecCSV) {
    using namespace proxy_test;
    SnapshotRegistry reg;
    reg.register_type_with_proxy<TextureAsset, TextureProxy>();

    World world;
    world.spawn().add(TextureAsset{ 999, "stone" });

    // Use Aurora to test Generic (Columnar/CSV) path
    auto manifest = AuroraArchive::create(world, reg, { AuroraArchive::Config::Format::Csv });
    
    // Check if the CSV contains the proxy data ("asset_path") instead of "handle"
    auto& entry = manifest.embed.at("arch_0");
    ASSERT_TRUE(entry.blob.has_value());
    EXPECT_NE(entry.blob->find("assets/stone.png"), std::string::npos);
    EXPECT_EQ(entry.blob->find("handle"), std::string::npos); // 'handle' should be hidden
}

namespace generic_codec_test {
struct Sample {
    double value;
    std::string label;
};
struct Proxy {
    using ReflectionType = double;
    double value;
    explicit Proxy(double v) : value(v) {}
    double reflection() const { return value; }
    static Proxy from(const Sample& s) { return Proxy(s.value); }
    Sample into() const { return {value, "proxy"}; }
};

void check_component_codec(const GenericCodec& codec, const Sample& source,
                           const std::string& expected_label) {
    auto generic = codec.to_generic(&source);
    World world;
    auto direct = world.spawn().entity;
    auto deferred = world.spawn().entity;
    ASSERT_TRUE(codec.from_generic(world, direct, generic).is_ok());
    CommandBuffer cmd(&world.index());
    ASSERT_TRUE(codec.from_generic_cmd(cmd, deferred, generic).is_ok());
    world.submit(cmd);
    for (auto entity : {direct, deferred}) {
        auto* loaded = world.entity(entity).get<Sample>();
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->value, source.value);
        EXPECT_EQ(loaded->label, expected_label);
    }

    auto invalid = world.spawn().entity;
    auto bad = reflect::Generic("not a component");
    EXPECT_TRUE(codec.from_generic(world, invalid, bad).is_err());
    EXPECT_TRUE(codec.from_generic_cmd(cmd, invalid, bad).is_err());
    world.submit(cmd);
    EXPECT_EQ(world.entity(invalid).get<Sample>(), nullptr);
}
}

TEST(ElysiaArchive, GenericComponentPreservesNonJsonValues) {
    using namespace generic_codec_test;
    SnapshotRegistry registry;
    ASSERT_TRUE(registry.register_type<Sample>().is_ok());
    // Infinity has no JSON numeric representation; embedded NUL must survive.
    Sample source{std::numeric_limits<double>::infinity(), std::string("a\0b", 3)};
    check_component_codec(*registry.find(std::string(TypeTraits<Sample>::name()))->generic,
                          source, source.label);
}

TEST(ElysiaArchive, GenericScalarProxyPreservesNonJsonValues) {
    using namespace generic_codec_test;
    SnapshotRegistry registry;
    ASSERT_TRUE((registry.register_type_with_proxy<Sample, Proxy>().is_ok()));
    Sample source{std::numeric_limits<double>::infinity(), "native"};
    check_component_codec(*registry.find(std::string(TypeTraits<Sample>::name()))->generic,
                          source, "proxy");
}

TEST(ElysiaArchive, GenericResourcePreservesNonJsonValues) {
    using namespace generic_codec_test;
    SnapshotRegistry registry;
    registry.register_resource<Sample>();
    const auto& codec = *registry.resource_factories().at(TypeTraits<Sample>::id).generic;
    Sample source{std::numeric_limits<double>::infinity(), std::string("x\0y", 3)};
    auto generic = codec.to_generic(&source);
    World world;
    ASSERT_TRUE(codec.from_generic(world, Entity{}, generic).is_ok());
    auto* loaded = world.get_resource<Sample>();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->value, source.value);
    EXPECT_EQ(loaded->label, source.label);
    EXPECT_TRUE(codec.from_generic(world, Entity{}, reflect::Generic("invalid")).is_err());
    EXPECT_EQ(world.get_resource<Sample>()->value, source.value);
}

TEST(ElysiaArchive, GenericColumnarPreservesNonJsonValues) {
    using namespace generic_codec_test;
    SnapshotRegistry registry;
    ASSERT_TRUE(registry.register_type<Sample>().is_ok());
    World source;
    source.spawn().add(Sample{std::numeric_limits<double>::infinity(), std::string("a\0b", 3)});
    auto manifest = AuroraArchive::create(source, registry, {AuroraArchive::Config::Format::Columnar});
    World target;
    CommandBuffer cmd(&target.index());
    ASSERT_TRUE(load_aurora(target, cmd, registry, manifest).is_ok());
    target.submit(cmd);
    int count = 0;
    target.query<Sample>().each([&](const Sample& sample) {
        EXPECT_EQ(sample.value, std::numeric_limits<double>::infinity());
        EXPECT_EQ(sample.label, std::string("a\0b", 3));
        ++count;
    });
    EXPECT_EQ(count, 1);
}
