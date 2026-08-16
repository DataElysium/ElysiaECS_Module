#include <gtest/gtest.h>
#include <string>
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
