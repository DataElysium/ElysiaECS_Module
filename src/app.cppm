
module;
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
export module elysia.app;

import elysia.world;
import elysia.schedule;
import elysia.observer;
import elysia.entity;
import elysia.meta;

export namespace elysia {

// Event Tags
struct OnAdd {};
struct OnRemove {};

/**
 * @brief Helper to build Observer registrations with fluent API.
 */
template <typename EventType, typename T> class ObserverBuilder {
public:
  ObserverBuilder(World &world) : world_(world) {}

  void run(std::function<void(Entity)> cb) {
    if constexpr (std::is_same_v<EventType, OnAdd>) {
      world_.observer().on_add<T>(std::move(cb));
    } else if constexpr (std::is_same_v<EventType, OnRemove>) {
      world_.observer().on_remove<T>(std::move(cb));
    }
  }

private:
  World &world_;
};

/**
 * @brief The App class is the central container for an Elysia application.
 * It integrates World, Scheduler, and high-level lifecycle management.
 */
class ELYSIA_API App {
public:
  App() = default;

  // --- Core Access ---
  World &world() { return world_; }
  Scheduler &scheduler() { return scheduler_; }

  // --- System Registration ---
  auto system(std::string name) { return scheduler_.system(std::move(name)); }

  template <typename Func>
  void add_startup_system(std::string name, Func &&func) {
    startup_scheduler_.system(std::move(name))
        .run(std::forward<Func>(func))
        .build(startup_scheduler_);
  }

  // --- Plugin System ---
  template <typename P> App &add_plugin(P &&p) {
    p.build(*this);
    return *this;
  }

  // --- Observer Registration ---
  template <typename Event, typename T> auto observer() {
    return ObserverBuilder<Event, T>(world_);
  }

  // --- Resource Management ---
  template <typename T> App &add_resource(T &&res) {
    world_.resources().add(std::forward<T>(res));
    return *this;
  }


  void init_serial() {
    if (!is_started_) {
      auto startup_exec = SerialExecutor::build_from(startup_scheduler_);
      startup_exec->run(&world_);
      is_started_ = true;
    }
    executor_ = SerialExecutor::build_from(scheduler_);
  }
  void init() { init_serial(); }
  void init_parallel() {
    if (!is_started_) {
      auto startup_exec = SerialExecutor::build_from(startup_scheduler_);
      startup_exec->run(&world_);
      is_started_ = true;
    }
    parallel_executor_ = TaskflowExecutor::build_from(scheduler_);
    use_parallel_ = true;
  }

  void update() {
    if (!is_started_) {
      init_serial();
    }

    if (use_parallel_)
      parallel_executor_->run(&world_);
    else
      executor_->run(&world_);
  }

private:
  World world_;
  Scheduler scheduler_;
  Scheduler startup_scheduler_; // 🌸 Startup phase
  std::unique_ptr<SerialExecutor> executor_;
  std::unique_ptr<TaskflowExecutor> parallel_executor_;
  bool is_started_ = false;
  bool use_parallel_ = false;
};

} // namespace elysia
