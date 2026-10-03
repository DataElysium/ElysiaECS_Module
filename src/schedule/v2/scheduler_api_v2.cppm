module;
#include <vector>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <taskflow/taskflow.hpp>
#include <numeric>
// ForkUnion 3.0.3 uses the pre-P4052 name. Limit the alias to its header.
#if defined(__cpp_lib_saturation_arithmetic) && __cpp_lib_saturation_arithmetic >= 202603L
#pragma push_macro("add_sat")
#define add_sat saturating_add
#include <forkunion.hpp>
#pragma pop_macro("add_sat")
#else
#include <forkunion.hpp>
#endif

export module elysia.schedule:executor;

import :base;
import :dag_builder;
import :graph_types;
import elysia.world;
import elysia.entity;
import elysia.query;
import elysia.meta;
import elysia.result;
import elysia.core;
import graph;
import graph.algo;

namespace fu = ashvardanian::forkunion;

export namespace elysia {

class ForkUnionExecutor : public schedule::SysExecutor {
    std::shared_ptr<ScheduleRuntime> runtime_;
public:
    ForkUnionExecutor() { pool_.try_spawn(std::thread::hardware_concurrency()); }
    static std::unique_ptr<ForkUnionExecutor> build_from(Scheduler& sched, schedule::CompileOptions options = {}) { return build_from(sched.snapshot(), options); }
    static std::unique_ptr<ForkUnionExecutor> build_from(std::shared_ptr<ScheduleRuntime> runtime, schedule::CompileOptions options = {}) {
        if (!runtime) throw std::invalid_argument("Executor requires a schedule runtime");
        auto exec = std::make_unique<ForkUnionExecutor>();
        exec->runtime_ = std::move(runtime);
        exec->compile(*exec->runtime_, options);
        return exec;
    }

    void init_all(World* world) {
        sched_->initialize(world);
        bound_world_ = world;
    }

    void run(World* world) {
        if (!world || world != bound_world_) init_all(world);
        try {
            for (const auto& wave : waves_) {
                if (!wave.parallel_systems.empty()) {
                    std::atomic<bool> failed{false};
                    std::exception_ptr error;
                    pool_.for_n_dynamic(wave.parallel_systems.size(), [&](size_t i) noexcept {
                        if (failed.load(std::memory_order_relaxed)) return;
                        try {
                            execute_system(world, wave.parallel_systems[i]);
                        } catch (...) {
                            // Only the first failing worker writes the exception.
                            if (!failed.exchange(true, std::memory_order_relaxed))
                                error = std::current_exception();
                        }
                    });
                    // for_n_dynamic joins the wave before the caller reads error.
                    if (error) std::rethrow_exception(error);
                }
                for (auto e : wave.exclusive_systems) execute_system(world, e);
            }
            flush_all_buffers(world);
        } catch (...) {
            runtime_->discard_commands();
            throw;
        }
    }

private:
    struct Wave { std::vector<entity_t> parallel_systems; std::vector<entity_t> exclusive_systems; };
    void compile(ScheduleRuntime& sched, schedule::CompileOptions options) {
        sched_ = &sched; waves_.clear();
        auto sg = schedule::compile_dag(sched.meta_world(), options);
        auto layers = graph::algo::kahn_layers(sg);
        for (const auto& layer : layers) {
            Wave wave;
            for (auto node_idx : layer) {
                entity_t e = sg.key(node_idx).entity;
                auto view = sched.meta_world().entity(e);
                if (auto* exec = view.get<schedule::SysExecutor>()) {
                    if (exec->threading == schedule::ThreadingModel::Exclusive || exec->affinity == schedule::ThreadAffinity::Caller) wave.exclusive_systems.push_back(e);
                    else wave.parallel_systems.push_back(e);
                }
            }
            if (!wave.parallel_systems.empty() || !wave.exclusive_systems.empty()) waves_.push_back(std::move(wave));
        }
    }

    // ⚠️ THREAD-SAFETY: Called from for_n_dynamic in parallel wave context.
    // world->update_query() may race on ComponentRegistry first-registration.
    // Systems sharing QueryState would also race on scanned_count writes.
    // 👉 CONSTRAINT: Queries are pre-prepared (types registered in build phase).
    //    Each system owns a unique QueryState via SysQuery component.
    // 👉 FUTURE: Add pre-prepare loop in init_all() for all SysQuery (done).
    void execute_system(World* world, entity_t sys_e) {
        auto view = sched_->meta_world().entity(sys_e);
        auto* exec = view.get<schedule::SysExecutor>();
        if (auto* q_comp = view.get<schedule::SysQuery>(); q_comp && q_comp->ptr) world->update_query(*static_cast<QueryState*>(q_comp->ptr.get()));
        auto* cmd_comp = view.get<schedule::SysCmdBuf>();
        void* cmd_ptr = cmd_comp ? cmd_comp->ptr.get() : nullptr;
        if (exec->kind == schedule::SpecialSystemKind::ApplyDeferred) flush_all_buffers(world);
        else if (exec->func) schedule::invoke_system(*exec, world, cmd_ptr, sched_->meta_world(), sys_e);
    }

    void flush_all_buffers(World* world) {
        sched_->meta_world().query<schedule::SysCmdBuf>().each([&](auto& cmd) { if (cmd.ptr && !cmd.ptr->headers().empty()) { world->submit(*cmd.ptr); cmd.ptr->clear(); } });
    }

    fu::flat_pool_t pool_; std::vector<Wave> waves_; ScheduleRuntime* sched_ = nullptr; World* bound_world_ = nullptr;
};

// Owned names make a completed profile safe to copy across a snapshot boundary.
// Read only between runs. System durations include query preparation; concurrent
// durations overlap and must never be summed to obtain executor wall time.
enum class TimingKind { Initialize, System, ApplyDeferred, FinalFlush };
struct SystemTiming {
    std::string name;
    double ms = 0;
    bool executed = false, failed = false;
    TimingKind kind = TimingKind::System;
};
struct ExecutionProfile {
    bool enabled = false, completed = false;
    double wall_ms = 0;
    std::vector<SystemTiming> systems;
};

namespace detail {
class ScheduleProfiler {
public:
    ExecutionProfile value;
    void prepare(World& meta, const std::vector<entity_t>& nodes) {
#ifdef ELYSIA_PERF_OVERLAY
        value = {};
        value.enabled = true;
        value.systems.push_back({"[initialize]", 0, false, false, TimingKind::Initialize});
        for (auto e : nodes) {
            auto view = meta.entity(e);
            auto* name = view.get<schedule::SysName>();
            auto* exec = view.get<schedule::SysExecutor>();
            value.systems.push_back({name ? name->value : std::to_string(e.value), 0, false, false,
                exec && exec->kind == schedule::SpecialSystemKind::ApplyDeferred
                    ? TimingKind::ApplyDeferred : TimingKind::System});
        }
        value.systems.push_back({"[final flush]", 0, false, false, TimingKind::FinalFlush});
#endif
    }
    template<class F> void measure(size_t slot, F&& fn) {
#ifdef ELYSIA_PERF_OVERLAY
        auto& sample = value.systems[slot];
        sample.executed = true;
        const auto begin = std::chrono::steady_clock::now();
        try { fn(); }
        catch (...) {
            sample.ms = elapsed(begin);
            sample.failed = true;
            throw;
        }
        sample.ms = elapsed(begin);
#else
        fn();
#endif
    }
    template<class F> void run(F&& fn) {
#ifdef ELYSIA_PERF_OVERLAY
        value.completed = false;
        for (auto& sample : value.systems) {
            sample.ms = 0;
            sample.executed = sample.failed = false;
        }
        const auto begin = std::chrono::steady_clock::now();
        try { fn(); }
        catch (...) { value.wall_ms = elapsed(begin); throw; }
        value.wall_ms = elapsed(begin);
        value.completed = true;
#else
        fn();
#endif
    }
private:
    static double elapsed(std::chrono::steady_clock::time_point begin) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
};
} // namespace detail

class SerialExecutor : public schedule::SysExecutor {
    std::shared_ptr<ScheduleRuntime> runtime_;
public:
    static std::unique_ptr<SerialExecutor> build_from(Scheduler& sched, schedule::CompileOptions options = {}) { return build_from(sched.snapshot(), options); }
    static std::unique_ptr<SerialExecutor> build_from(std::shared_ptr<ScheduleRuntime> runtime, schedule::CompileOptions options = {}) {
        if (!runtime) throw std::invalid_argument("Executor requires a schedule runtime");
        auto exec = std::make_unique<SerialExecutor>();
        exec->runtime_ = std::move(runtime);
        exec->compile(*exec->runtime_, options);
        return exec;
    }
    void init_all(World* world) {
        sched_->initialize(world);
        bound_world_ = world;
    }
    void run(World* world) {
        profiler_.run([&] {
            try {
                if (!world || world != bound_world_)
                    profiler_.measure(0, [&] { init_all(world); });
                for (size_t i = 0; i < plan_.size(); ++i) {
                    profiler_.measure(i + 1, [&] {
                        auto e = plan_[i];
                        auto view = meta_world_->entity(e);
                        auto* exec = view.get<schedule::SysExecutor>();
                        if (auto* q = view.get<schedule::SysQuery>(); q && q->ptr)
                            world->update_query(*static_cast<QueryState*>(q->ptr.get()));
                        auto* cmd = view.get<schedule::SysCmdBuf>();
                        if (exec->kind == schedule::SpecialSystemKind::ApplyDeferred) flush_all_buffers(world);
                        else if (exec->func)
                            schedule::invoke_system(*exec, world, cmd ? cmd->ptr.get() : nullptr, *meta_world_, e);
                    });
                }
                profiler_.measure(plan_.size() + 1, [&] { flush_all_buffers(world); });
            } catch (...) {
                runtime_->discard_commands();
                throw;
            }
        });
    }
    using SysTime = SystemTiming;
    const std::vector<SysTime>& sys_times() const { return profiler_.value.systems; }
    const ExecutionProfile& profile() const { return profiler_.value; }
private:
    void compile(ScheduleRuntime& sched, schedule::CompileOptions options) {
        sched_ = &sched; meta_world_ = &sched.meta_world(); plan_.clear();
        auto sg = schedule::compile_dag(*meta_world_, options);
        auto layers = graph::algo::kahn_layers(sg);
        for (const auto& layer : layers) {
            for (auto node_idx : layer) {
                entity_t e = sg.key(node_idx).entity;
                if (meta_world_->entity(e).get<schedule::SysExecutor>()) plan_.push_back(e);
            }
        }
        profiler_.prepare(*meta_world_, plan_);
    }
    void flush_all_buffers(World* world) { meta_world_->query<schedule::SysCmdBuf>().each([&](auto& cmd) { if (cmd.ptr && !cmd.ptr->headers().empty()) { world->submit(*cmd.ptr); cmd.ptr->clear(); } }); }
    ScheduleRuntime* sched_ = nullptr; World* meta_world_ = nullptr; std::vector<entity_t> plan_; World* bound_world_ = nullptr;
    detail::ScheduleProfiler profiler_;
};

class TaskflowExecutor : public schedule::SysExecutor {
    std::shared_ptr<ScheduleRuntime> runtime_;
public:
    static std::unique_ptr<TaskflowExecutor> build_from(Scheduler& sched, schedule::CompileOptions options = {}) { return build_from(sched.snapshot(), options); }
    static std::unique_ptr<TaskflowExecutor> build_from(std::shared_ptr<ScheduleRuntime> runtime, schedule::CompileOptions options = {}) {
        if (!runtime) throw std::invalid_argument("Executor requires a schedule runtime");
        auto exec = std::make_unique<TaskflowExecutor>();
        exec->runtime_ = std::move(runtime);
        exec->compile(*exec->runtime_, options);
        return exec;
    }
    void init_all(World* world) {
        sched_->initialize(world);
        bound_world_ = world;
    }
    void run(World* world) {
        profiler_.run([&] {
            try {
                if (!world || world != bound_world_)
                    profiler_.measure(0, [&] { init_all(world); });
                current_world_ = world;
                // get waits for cancellation/running tasks and rethrows task failures.
                for (size_t i = 0; i < taskflows_.size(); ++i) {
                    if (taskflows_[i]->num_tasks()) executor_.run(*taskflows_[i]).get();
                    if (i < caller_systems_.size()) {
                        auto [entity, slot] = caller_systems_[i];
                        execute_system(entity, slot);
                    }
                }
                profiler_.measure(final_slot_, [&] { flush_all_buffers(world); });
            } catch (...) {
                current_world_ = nullptr;
                runtime_->discard_commands();
                throw;
            }
            current_world_ = nullptr;
        });
    }
    using SysTime = SystemTiming;
    const std::vector<SysTime>& sys_times() const { return profiler_.value.systems; }
    const ExecutionProfile& profile() const { return profiler_.value; }
private:
    void execute_system(entity_t e, size_t slot) {
        // Every compiled node owns a distinct, preallocated slot. No worker lock
        // or allocation is needed, and cancelled nodes remain unexecuted.
        profiler_.measure(slot, [&] {
            auto* w = current_world_;
            auto view = meta_ptr_->entity(e);
            auto* exec = view.get<schedule::SysExecutor>();
            if (!exec) return;
            if (auto* q = view.get<schedule::SysQuery>(); q && q->ptr)
                w->update_query(*static_cast<QueryState*>(q->ptr.get()));
            auto* cmd = view.get<schedule::SysCmdBuf>();
            if (exec->kind == schedule::SpecialSystemKind::ApplyDeferred) flush_all_buffers(w);
            else if (exec->func) schedule::invoke_system(*exec, w, cmd ? cmd->ptr.get() : nullptr, *meta_ptr_, e);
        });
    }
    void compile(ScheduleRuntime& sched, schedule::CompileOptions options) {
        sched_ = &sched; meta_ptr_ = &sched.meta_world();
        auto sg = schedule::compile_dag(*meta_ptr_, options);
        auto order = graph::algo::kahn_layers(sg);
        taskflows_.clear(); caller_systems_.clear();
        std::vector<entity_t> profiled_nodes;
        for (size_t i = 0; i < sg.node_count(); ++i) profiled_nodes.push_back(sg.key(i).entity);
        profiler_.prepare(*meta_ptr_, profiled_nodes);
        final_slot_ = profiled_nodes.size() + 1;
        std::vector<std::vector<size_t>> runs(1);
        // Caller nodes are explicit join points. They never enter a worker taskflow.
        for (auto i : order.nodes_sorted) {
            auto* exec = meta_ptr_->entity(sg.key(i).entity).get<schedule::SysExecutor>();
            if (exec && exec->affinity == schedule::ThreadAffinity::Caller) {
                caller_systems_.push_back({sg.key(i).entity, i + 1});
                runs.emplace_back();
            } else runs.back().push_back(i);
        }
        for (const auto& run : runs) {
            auto flow = std::make_unique<tf::Taskflow>();
            std::unordered_map<size_t, tf::Task> tasks;
            for (auto i : run) {
                auto e = sg.key(i).entity;
                auto* name = meta_ptr_->entity(e).get<schedule::SysName>();
                tasks[i] = flow->emplace([this, e, i] { execute_system(e, i + 1); }).name(name ? name->value : std::to_string(i));
            }
            // Preserve ordinary DAG concurrency and exclusive boundaries within each run.
            const auto count = sg.node_count();
            std::vector<std::vector<size_t>> sections(1);
            std::vector<size_t> barriers, section_of(count, count + 1);
            for (auto i : run) {
                auto* exec = meta_ptr_->entity(sg.key(i).entity).get<schedule::SysExecutor>();
                if (exec && (exec->threading == schedule::ThreadingModel::Exclusive ||
                             exec->kind == schedule::SpecialSystemKind::ApplyDeferred)) {
                    barriers.push_back(i); sections.emplace_back();
                } else {
                    section_of[i] = sections.size() - 1;
                    sections.back().push_back(i);
                }
            }
            std::vector<bool> has_predecessor(count), has_successor(count);
            for (auto i : run) {
                if (section_of[i] == count + 1) continue;
                for (const auto& edge : sg.out_edges(i)) {
                    if (section_of[i] != section_of[edge.to]) continue;
                    tasks.at(i).precede(tasks.at(edge.to));
                    has_successor[i] = has_predecessor[edge.to] = true;
                }
            }
            for (size_t section = 0; section < sections.size(); ++section) {
                if (sections[section].empty() && section > 0 && section < barriers.size())
                    tasks.at(barriers[section - 1]).precede(tasks.at(barriers[section]));
                for (auto i : sections[section]) {
                    if (section > 0 && !has_predecessor[i]) tasks.at(barriers[section - 1]).precede(tasks.at(i));
                    if (section < barriers.size() && !has_successor[i]) tasks.at(i).precede(tasks.at(barriers[section]));
                }
            }
            taskflows_.push_back(std::move(flow));
        }
    }
    void flush_all_buffers(World* world) { meta_ptr_->query<schedule::SysCmdBuf>().each([&](auto& c) { if (c.ptr && !c.ptr->headers().empty()) { world->submit(*c.ptr); c.ptr->clear(); } }); }
    ScheduleRuntime* sched_ = nullptr;
    tf::Executor executor_;
    std::vector<std::unique_ptr<tf::Taskflow>> taskflows_;
    std::vector<std::pair<entity_t, size_t>> caller_systems_; World* current_world_ = nullptr; World* meta_ptr_ = nullptr; World* bound_world_ = nullptr;
    detail::ScheduleProfiler profiler_;
    size_t final_slot_ = 0;
};

} // namespace elysia
