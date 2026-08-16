module;
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

export module elysia.schedule:base;

import elysia.schedule.components;
import :traits;
import :dag_builder;
import elysia.world;
import elysia.entity;
import elysia.query;
import elysia.meta;
import elysia.result;

export namespace elysia {

class ELYSIA_API Scheduler;

namespace schedule {

class ELYSIA_API SystemBuilder {
public:
  explicit SystemBuilder(std::string name,
                         elysia::Scheduler *default_target = nullptr)
      : name_(std::move(name)), default_target_(default_target) {}

  SystemBuilder &after(const std::string &target) {
    deps_.afters.push_back(target);
    return *this;
  }
  SystemBuilder &before(const std::string &target) {
    deps_.befores.push_back(target);
    return *this;
  }
  SystemBuilder &in_set(const std::string &target) {
    deps_.in_set = target;
    return *this;
  }
  SystemBuilder &kind(SpecialSystemKind k) {
    kind_ = k;
    return *this;
  }
  SystemBuilder &exclusive(bool e = true) {
    threading_ = e ? ThreadingModel::Exclusive : ThreadingModel::Parallel;
    return *this;
  }

  template <typename Func> SystemBuilder &run(Func &&f);
  template <typename T> SystemBuilder &run();

  void build(elysia::Scheduler &sched);
  void build();

  const std::string &name() const { return name_; }

private:
  struct Deps {
    std::vector<std::string> afters;
    std::vector<std::string> befores;
    std::string in_set;
  };
  std::string name_;
  elysia::Scheduler *default_target_;
  Deps deps_;
  ThreadingModel threading_ = ThreadingModel::Parallel;
  SpecialSystemKind kind_ = SpecialSystemKind::None;
  std::function<void(elysia::Scheduler *, entity_t, Deps)> build_action_;
};

struct PhaseProxy {
  elysia::Scheduler *sched;
  std::string phase_name;
  template <typename Func> entity_t add(const std::string &name, Func &&f);
};
} // namespace schedule

class ELYSIA_API Scheduler {
public:
  Scheduler() {
    auto &reg = meta_world_.graph().registry();
    reg.ensure_registered(get_type_info_ptr<schedule::SysName>());
    reg.ensure_registered(get_type_info_ptr<schedule::SysExecutor>());
    reg.ensure_registered(get_type_info_ptr<schedule::SysQuery>());
    reg.ensure_registered(get_type_info_ptr<schedule::SysCmdBuf>());
    reg.ensure_registered(get_type_info_ptr<schedule::SysFactory>());
  }

  schedule::SystemBuilder system(std::string name) {
    return schedule::SystemBuilder(std::move(name), this);
  }
  schedule::PhaseProxy phase(std::string name) {
    auto e = resolve(name);
    meta_world_.entity(e).add(schedule::SetTag{});
    return {this, std::move(name)};
  }
  template <typename... Builders> void add(Builders &&...builders) {
    (builders.build(*this), ...);
  }
  template <typename... Args> void chain(Args &&...args) {
    std::string last_name = "";
    (
        [&](auto &&item) {
          std::string current_name;
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_convertible_v<T, std::string>) {
            current_name = static_cast<std::string>(item);
            meta_world_.entity(resolve(current_name)).add(schedule::SetTag{});
          } else if constexpr (std::is_same_v<T, schedule::SyncMarker>) {
            current_name = "__sync_" + std::to_string(anon_counter_++);
            system(current_name)
                .kind(schedule::SpecialSystemKind::ApplyDeferred)
                .build();
          } else if constexpr (std::is_same_v<T, schedule::SystemBuilder>) {
            current_name = item.name();
            item.build(*this);
          }
          if (!last_name.empty() && !current_name.empty())
            add_dependency(current_name, last_name);
          last_name = current_name;
        }(std::forward<Args>(args)),
        ...);
  }

  entity_t resolve(const std::string &name) {
    if (name.empty())
      return entity_t{};
    if (auto it = symbol_table_.find(name); it != symbol_table_.end())
      return it->second;
    auto e = meta_world_.spawn().add(schedule::SysName{name}).entity;
    symbol_table_[name] = e;
    return e;
  }

  entity_t add_system_entity(std::string name) {
    auto e = resolve(name);
    meta_world_.entity(e).add(schedule::SystemTag{}).add(schedule::SysStatus{});
    return e;
  }

  template <typename Func> void add_system(std::string name, Func &&func) {
    system(std::move(name)).run(std::forward<Func>(func)).build(*this);
  }

  void add_dependency(const std::string &src_name,
                      const std::string &target_name) {
    auto src = resolve(src_name);
    auto target = resolve(target_name);
    auto view = meta_world_.entity(src);
    if (auto *dep = view.get<schedule::DependsOn>()) {
      if (std::find(dep->targets.begin(), dep->targets.end(), target) ==
          dep->targets.end())
        dep->targets.push_back(target);
    } else
      view.add(schedule::DependsOn{{target}});
  }

  void build() {}
  World &meta_world() { return meta_world_; }

private:
  World meta_world_;
  std::unordered_map<std::string, entity_t> symbol_table_;
  uint32_t anon_counter_ = 0;
};

// Implementations
namespace schedule {
// =============================================================================
// run(Func&&) — adapter-based dispatch (replaces 7 if-constexpr branches)
// =============================================================================
template <typename Func> SystemBuilder &SystemBuilder::run(Func &&f) {
  build_action_ = [f_orig = std::forward<Func>(f), name = name_, k = kind_,
                   th =
                       threading_](elysia::Scheduler *sched, entity_t e,
                                   schedule::SystemBuilder::Deps deps) mutable {
    using RawFunc = std::decay_t<Func>;
    using Adapter = system_adapter_t<RawFunc>;

    auto query_ptr = Adapter::template make_query<RawFunc>();
    auto executor =
        Adapter::template make_executor<RawFunc>(std::move(f_orig), query_ptr);

    // ── unified tail: register components onto meta-world entity ──
    auto view = sched->meta_world().entity(e);

    // WorldMutSystem receives a mutable World* — auto-enforce Exclusive
    // to prevent data races on structural mutations (spawn/despawn/add/remove).
    ThreadingModel effective_th = th;
    if constexpr (WorldMutSystem<RawFunc>) {
      effective_th = ThreadingModel::Exclusive;
    }

    view.add(SysExecutor{std::move(executor), effective_th, k});
    if (query_ptr)
      view.add(SysQuery{query_ptr});

    // ⚠️ THREAD-SAFETY: WorldSystem/WorldMutSystem do NOT receive SysCmdBuf.
    // They may call world.spawn() directly → NOT thread-safe (EntityIndex
    // free-list). This is enforced by auto-Exclusive for WorldMutSystem.
    // 👉 CONSTRAINT: WorldMutSystem(World*) → auto-Exclusive (line 197).
    //    WorldReadSystem(const World*) → cannot spawn (const).
    // 👉 FUTURE: cmd.spawn() provides atomic parallel-safe spawn. Systems
    //    using cmd.spawn() may relax auto-Exclusive in the future.
    constexpr bool has_cmd = has_cmd_buf_v<RawFunc>;
    if constexpr (has_cmd || IterSystem<RawFunc> || WorldCmdSystem<RawFunc> || WorldViewCmdSystem<RawFunc>) {
      view.add(SysCmdBuf{std::make_shared<CommandBuffer>()});
    }

    for (const auto &t : deps.afters)
      sched->add_dependency(name, t);
    for (const auto &t : deps.befores)
      sched->add_dependency(t, name);
    if (!deps.in_set.empty()) {
      auto set_e = sched->resolve(deps.in_set);
      sched->meta_world().entity(set_e).add(SetTag{});
      view.add(InSet{set_e});
    }
  };
  return *this;
}

// =============================================================================
// run<T>() — type-based system (uses SysFactory for lazy init)
// =============================================================================
template <typename T> SystemBuilder &SystemBuilder::run() {
  // ── Detect system kind ────────────────────────────────────────────────────
  constexpr bool takes_world_cmd =
    requires(T& inst, World* w, CommandBuffer* c) { inst(w, c); };
  constexpr bool takes_world =
    !takes_world_cmd && requires(T& inst, World* w) { inst(w); };
  constexpr bool takes_wv_cmd =
    !takes_world_cmd && !takes_world &&
    requires(T& inst, WorldView wv, CommandBuffer* c) { inst(wv, c); };
  constexpr bool takes_wv =
    !takes_wv_cmd && !takes_world_cmd && !takes_world &&
    requires(T& inst, WorldView wv) { inst(wv); };

  build_action_ = [th = threading_, k = kind_,
                   name = name_](elysia::Scheduler *sched, entity_t e,
                                 schedule::SystemBuilder::Deps deps) {
    auto view = sched->meta_world().entity(e);
    view.add(SysFactory{[](World *w) -> RunClosure {
      T instance;
      if constexpr (requires { instance.init(w); })
        instance.init(w);
      return [inst = std::move(instance)](
                  World *wr, void *, void *cmd_ptr) mutable -> Result<void> {
        if constexpr (takes_world_cmd) {
          assert(cmd_ptr != nullptr && "Elysia Error: CommandBuffer is null in takes_world_cmd!");
          inst(wr, static_cast<CommandBuffer *>(cmd_ptr));
          return Result<void>::ok();
        } else if constexpr (takes_world) {
          inst(wr);
          return Result<void>::ok();
        } else if constexpr (takes_wv_cmd) {
          assert(cmd_ptr != nullptr && "Elysia Error: CommandBuffer is null in takes_wv_cmd!");
          inst(WorldView{wr}, static_cast<CommandBuffer *>(cmd_ptr));
          return Result<void>::ok();
        } else if constexpr (takes_wv) {
          if constexpr (std::is_void_v<decltype(inst(WorldView{wr}))>) {
            inst(WorldView{wr});
            return Result<void>::ok();
          } else {
            return inst(WorldView{wr});
          }
        } else {
          return Result<void>::ok();
        }
      };
    }});

    // World* systems auto-Exclusive (direct structural access).
    // WorldView systems stay at user-specified threading (default Parallel).
    ThreadingModel effective_th = th;
    if constexpr (takes_world || takes_world_cmd) {
      effective_th = ThreadingModel::Exclusive;
    }

    if constexpr (takes_wv_cmd || takes_world_cmd) {
      view.add(SysCmdBuf{std::make_shared<CommandBuffer>()});
    }
    view.add(SysExecutor{nullptr, effective_th, k});
    for (const auto &t : deps.afters)
      sched->add_dependency(name, t);
    for (const auto &t : deps.befores)
      sched->add_dependency(t, name);
    if (!deps.in_set.empty())
      view.add(InSet{sched->resolve(deps.in_set)});
  };
  return *this;
}

void SystemBuilder::build(elysia::Scheduler &sched) {
  if (build_action_) {
    build_action_(&sched, sched.add_system_entity(name_), deps_);
  } else if (kind_ == SpecialSystemKind::ApplyDeferred) {
    entity_t e = sched.add_system_entity(name_);
    sched.meta_world()
        .entity(e)
        .add(ApplyDeferredTag{})
        .add(SysExecutor{nullptr, ThreadingModel::Exclusive,
                         SpecialSystemKind::ApplyDeferred});
    for (const auto &t : deps_.afters)
      sched.add_dependency(name_, t);
    for (const auto &t : deps_.befores)
      sched.add_dependency(t, name_);
  }
}
void SystemBuilder::build() {
  if (default_target_)
    build(*default_target_);
}
template <typename Func>
entity_t PhaseProxy::add(const std::string &name, Func &&f) {
  sched->system(name).in_set(phase_name).run(std::forward<Func>(f)).build();
  return sched->resolve(name);
}
} // namespace schedule

} // namespace elysia
