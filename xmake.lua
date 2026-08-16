set_project("elysia_ecs_module")
set_version("0.1.1")
set_languages("cxxlatest")
set_toolchains("clang")

add_rules("mode.debug", "mode.release")

-- Optimizations
option("lto")
    set_default(true)
    set_showmenu(true)
    set_description("Enable link-time optimization (release)")
option_end()
option("sections")
    set_default(true)
    set_showmenu(true)
    set_description("Enable removing dead code and symbols in release build")
option_end()
option("native")
    set_default(true)
    set_showmenu(true)
    set_description("Enable -march=native/-mtune=native (release)")
option_end()
option("shared")
    set_default(true)
    set_showmenu(true)
    set_description("Build ElysiaShared and dynamic plugins")
option_end()
option("archive")
    set_default(false)
    set_showmenu(true)
    set_description("Enable optional archive/prefab persistence modules")
option_end()
option("bench")
    set_default(false)
    set_showmenu(true)
    set_description("Enable benchmarks")
option_end()
option("capnproto")
    set_default(false)
    set_showmenu(true)
    set_description("Enable Cap'n Proto support")
option_end()
option("perf_overlay")
    set_default(true)
    set_showmenu(true)
    set_description("Enable per-system performance overlay (ELYSIA_PERF_OVERLAY)")
option_end()

if is_mode("release") then
    if has_config("lto") then
        set_policy("build.optimization.lto", true)
    end
    set_optimize("fastest")
    if has_config("sections") then
        add_cxflags("-ffunction-sections", "-fdata-sections", {tools = {"clang", "gcc"}})
        add_ldflags("-Wl,--gc-sections", "-Wl,--print-gc-sections", {tools = {"clang", "gcc"}})
        add_cxflags("-fno-rtti", {tools = {"clang","gcc"}})
    end
    if has_config("native") then
        add_cxxflags("-march=native -mtune=native", {tools = "clang"})
        add_cflags("-march=native -mtune=native", {tools = "clang"})
    end
end
if is_host("windows") then
    if is_mode("debug") then
        set_runtimes("MDd")
    else
        set_runtimes("MD")
    end
end

add_requires("gtest", "taskflow", "fmt", "nameof")
add_requires("entt", "flecs", "raylib")
add_requires("magic_enum", {configs = {modules = true}})

if has_config("archive") then
    add_requires("reflect-cpp 0.24.0", {configs = {msgpack = true, flatbuffers = true, toml = true}})
end

includes("modules")

local function add_core_files()
    add_files("src/*.cppm", {public = true})
    add_files("src/command/*.cppm", {public = true})
    add_files("src/schedule/v2/*.cppm", {public = true})
    add_files("src/plugins/*.cppm", {public = true})
    add_files("src/*.cpp")
    add_files("src/command/*.cpp")
end

local function add_archive_files()
    add_files("src/archive/*.cppm", {public = true})
    add_files("src/archive/codec/*.cppm", {public = true})
end

local function add_core_test_files()
    for _, file in ipairs(os.files("tests/*.cpp")) do
        local filename = path.filename(file)
        if not filename:find("^elysia_archive") then
            add_files(file)
        end
    end
end

local function add_archive_test_files()
    add_files("tests/main.cpp")
    for _, file in ipairs(os.files("tests/elysia_archive*.cpp")) do
        add_files(file)
    end
end

local function add_shared_core_test_files()
    add_files("tests/main.cpp")
    add_files("tests/elysia_world_test.cpp")
    add_files("tests/elysia_query_entity_test.cpp")
    add_files("tests/elysia_filter_test.cpp")
end

local function add_core_settings(api_define)
    set_languages("cxxlatest")
    set_policy("build.c++.modules", true)
    add_defines(api_define)
    add_includedirs("src")
    add_deps("std_module", "Graph")
    add_packages("nameof", "taskflow")

    if has_config("perf_overlay") then
        add_defines("ELYSIA_PERF_OVERLAY")
    end

    add_includedirs("thirdparty/ForkUnion/include", {public = true})
end

local function add_archive_settings(core_api_define, archive_api_define, core_dep)
    set_languages("cxxlatest")
    set_policy("build.c++.modules", true)
    add_defines(core_api_define)
    add_defines(archive_api_define)
    add_includedirs("src")
    add_deps(core_dep)
    add_packages("reflect-cpp")

    if has_config("capnproto") then
        add_packages("capnproto")
        add_defines("ELYSIA_ENABLE_CAPNPROTO")
        if is_plat("windows") then
            add_syslinks("Advapi32", "Crypt32", "Ws2_32")
        end
    end
end

target("Elysia")
    set_kind("static")
    add_core_settings("ELYSIA_API=")
    add_core_files()
target_end()

if has_config("shared") then
    target("ElysiaShared")
        set_kind("shared")
        if is_plat("windows") then
            add_core_settings("ELYSIA_API=__declspec(dllexport)")
        else
            add_core_settings("ELYSIA_API=__attribute__((visibility(\"default\")))")
        end
        add_core_files()
    target_end()
end

if has_config("archive") then
    target("ElysiaArchive")
        set_kind("static")
        add_archive_settings("ELYSIA_API=", "ELYSIA_ARCHIVE_API=", "Elysia")
        add_archive_files()
    target_end()
end

target("elysia_tests")
    set_kind("binary")
    set_languages("cxxlatest")
    set_policy("build.c++.modules", true)
    add_deps("Elysia")
    add_defines("ELYSIA_API=")
    add_packages("gtest", "entt", "taskflow")
    add_core_test_files()
target_end()

if has_config("shared") then
    target("elysia_tests_shared")
        set_kind("binary")
        set_languages("cxxlatest")
        set_policy("build.c++.modules", true)
        add_deps("ElysiaShared")
        if is_plat("windows") then
            add_defines("ELYSIA_API=__declspec(dllimport)")
        else
            add_defines("ELYSIA_API=")
        end
        add_packages("gtest", "entt", "taskflow")
        add_shared_core_test_files()
    target_end()
end

if has_config("archive") then
    target("elysia_archive_tests")
        set_kind("binary")
        set_languages("cxxlatest")
        set_policy("build.c++.modules", true)
        add_deps("ElysiaArchive")
        add_defines("ELYSIA_API=")
        add_packages("gtest", "entt", "taskflow")
        add_archive_test_files()
    target_end()
end

if has_config("bench") then
    target("elysia_bench")
        set_kind("binary")
        set_languages("cxxlatest")
        set_policy("build.c++.modules", true)
        add_deps("Elysia")
        add_defines("ELYSIA_API=")
        add_packages("entt", "flecs")
        add_files("bench/command_battle.cpp")
    target_end()
    target("elysia_1M_bench")
        set_kind("binary")
        set_languages("cxxlatest")
        set_policy("build.c++.modules", true)
        add_deps("Elysia")
        add_defines("ELYSIA_API=")
        add_packages("entt", "flecs")
        add_files("bench/main.cpp")
    target_end()
    if has_config("archive") then
        target("codec_bench")
            set_kind("binary")
            set_languages("cxxlatest")
            set_policy("build.c++.modules", true)
            add_deps("ElysiaArchive")
            add_defines("ELYSIA_API=")
            add_files("bench/codec_bench.cpp")
        target_end()
    end
end

target("elysia_boids")
    set_kind("binary")
    set_languages("cxxlatest")
    set_policy("build.c++.modules", true)
    add_deps("Elysia")
    add_defines("ELYSIA_API=")
    add_packages("raylib")
    add_files("examples/boids_app.cpp")
target_end()

target("spacewar")
    set_kind("binary")
    set_languages("cxxlatest")
    set_policy("build.c++.modules", true)
    add_deps("Elysia")
    add_defines("ELYSIA_API=")
    add_packages("raylib")
    add_files("examples/spacewar_app.cpp")
target_end()

target("ElysiaHelloWorld")
    set_kind("binary")
    set_languages("cxxlatest")
    set_policy("build.c++.modules", true)
    add_deps("Elysia")
    add_defines("ELYSIA_API=")
    add_files("examples/01_hello_world.cpp")
target_end()

target("simple_app")
    set_kind("binary")
    set_languages("cxxlatest")
    set_policy("build.c++.modules", true)
    add_deps("Elysia")
    add_defines("ELYSIA_API=")
    add_packages("raylib")
    add_files("examples/simple_app.cpp")
target_end()