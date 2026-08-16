module;
#include <functional>
#include <vector>
#include <memory>
#include <string>

export module elysia.schedule.components;

import elysia.entity;
import elysia.result;
import elysia.world;

// WorldView is defined in elysia.world (imported above) — no re-definition needed.

export namespace elysia::schedule {

enum class ThreadingModel { Parallel, Exclusive };
enum class SpecialSystemKind { None, ApplyDeferred };

struct SysName { std::string value; };
struct SystemTag {};
struct SetTag {};
struct ApplyDeferredTag {};

struct DependsOn { std::vector<entity_t> targets; };
struct InSet { entity_t target; };

struct SysExecutor {
    std::function<Result<void>(World*, void* resources, void* commands)> func;
    ThreadingModel threading = ThreadingModel::Parallel;
    SpecialSystemKind kind = SpecialSystemKind::None;
};

using RunClosure = std::function<Result<void>(World*, void*, void*)>;
struct SysFactory {
    std::function<RunClosure(World*)> func;
};

struct SysStatus {
    bool initialized = false;
    bool running = false;
};

struct SysQuery { std::shared_ptr<void> ptr; };
struct SysCmdBuf { std::shared_ptr<CommandBuffer> ptr; };

struct SyncMarker {};
inline constexpr SyncMarker Sync{};

} // namespace elysia::schedule
