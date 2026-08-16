module;
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <taskflow/taskflow.hpp>
#include <fork_union.hpp>

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

namespace fu = ashvardanian::fork_union;

export namespace elysia {

class ForkUnionExecutor : public schedule::SysExecutor {
public:
    ForkUnionExecutor() { pool_.try_spawn(std::thread::hardware_concurrency()); }
    static std::unique_ptr<ForkUnionExecutor> build_from(Scheduler& sched) { auto exec = std::make_unique<ForkUnionExecutor>(); exec->compile(sched); return exec; }

    void init_all(World* world) {
        auto& meta = sched_->meta_world();
        meta.query<schedule::SysFactory, schedule::SysExecutor>().each([&](auto& factory, auto& exec) { exec.func = factory.func(world); });

        // Pre-prepare all queries to guarantee is_prepared==true before parallel waves.
        meta.query<schedule::SysQuery>().each([&](auto& q) {
            if (q.ptr) world->update_query(*static_cast<QueryState*>(q.ptr.get()));
        });

        meta.query<schedule::SysCmdBuf>().each([&](auto& cmd) {
            if (!cmd.ptr) cmd.ptr = std::make_shared<CommandBuffer>(&world->index());
            else cmd.ptr->set_index(&world->index());
        });
        bound_world_ = world;
    }

    void run(World* world) {
        if (world != bound_world_) init_all(world);
        for (const auto& wave : waves_) {
            if (!wave.parallel_systems.empty()) {
                pool_.for_n_dynamic(wave.parallel_systems.size(), [&](size_t i) noexcept { execute_system(world, wave.parallel_systems[i]); });
            }
            for (auto e : wave.exclusive_systems) execute_system(world, e);
        }
        flush_all_buffers(world);
    }

private:
    struct Wave { std::vector<entity_t> parallel_systems; std::vector<entity_t> exclusive_systems; };
    void compile(Scheduler& sched) {
        sched_ = &sched; waves_.clear();
        auto sg = schedule::build_dag(sched.meta_world());
        auto layers = graph::algo::kahn_layers(sg);
        for (const auto& layer : layers) {
            Wave wave;
            for (auto node_idx : layer) {
                entity_t e = sg.key(node_idx).entity;
                auto view = sched.meta_world().entity(e);
                if (auto* exec = view.get<schedule::SysExecutor>()) {
                    if (exec->threading == schedule::ThreadingModel::Exclusive) wave.exclusive_systems.push_back(e);
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
        if (auto* q_comp = view.get<schedule::SysQuery>()) world->update_query(*static_cast<QueryState*>(q_comp->ptr.get()));
        auto* cmd_comp = view.get<schedule::SysCmdBuf>();
        void* cmd_ptr = cmd_comp ? cmd_comp->ptr.get() : nullptr;
        if (exec->kind == schedule::SpecialSystemKind::ApplyDeferred) flush_all_buffers(world);
        else if (exec->func) exec->func(world, nullptr, cmd_ptr);
    }

    void flush_all_buffers(World* world) {
        sched_->meta_world().query<schedule::SysCmdBuf>().each([&](auto& cmd) { if (cmd.ptr && !cmd.ptr->headers().empty()) { world->submit(*cmd.ptr); cmd.ptr->clear(); } });
    }

    fu::basic_pool_t pool_; std::vector<Wave> waves_; Scheduler* sched_ = nullptr; World* bound_world_ = nullptr;
};

class SerialExecutor : public schedule::SysExecutor {
public:
    static std::unique_ptr<SerialExecutor> build_from(Scheduler& sched) { auto exec = std::make_unique<SerialExecutor>(); exec->compile(sched); return exec; }   
    void init_all(World* world) {
        auto& meta = *meta_world_;
        meta.query<schedule::SysFactory, schedule::SysExecutor>().each([&](auto& factory, auto& exec) { exec.func = factory.func(world); });
        meta.query<schedule::SysQuery>().each([&](auto& q) { if (q.ptr) world->update_query(*static_cast<QueryState*>(q.ptr.get())); });
        meta.query<schedule::SysCmdBuf>().each([&](auto& cmd) {
            if (!cmd.ptr) cmd.ptr = std::make_shared<CommandBuffer>(&world->index());
            else cmd.ptr->set_index(&world->index());
        });
        bound_world_ = world;
    }
    void run(World* world) {
        if (world != bound_world_) init_all(world);
#ifdef ELYSIA_PERF_OVERLAY
        sys_times_.clear();
        using Clock = std::chrono::steady_clock;
        using Ms    = std::chrono::duration<float, std::milli>;
#endif
        for (auto e : plan_) {
            auto view = meta_world_->entity(e);
            auto* exec = view.get<schedule::SysExecutor>();
            if (auto* q_comp = view.get<schedule::SysQuery>()) world->update_query(*static_cast<QueryState*>(q_comp->ptr.get()));
            auto* cmd_comp = view.get<schedule::SysCmdBuf>();
            void* cmd_ptr = cmd_comp ? cmd_comp->ptr.get() : nullptr;
            if (exec->kind == schedule::SpecialSystemKind::ApplyDeferred) {
                flush_all_buffers(world);
            } else if (exec->func) {
#ifdef ELYSIA_PERF_OVERLAY
                auto t0 = Clock::now();
                exec->func(world, nullptr, cmd_ptr);
                float ms = Ms(Clock::now() - t0).count();
                const char* name = "";
                if (auto* n = view.get<schedule::SysName>()) name = n->value.c_str();
                sys_times_.push_back({name, ms});
#else
                exec->func(world, nullptr, cmd_ptr);
#endif
            }
        }
        flush_all_buffers(world);
    }
    struct SysTime { const char* name; float ms; };
    const std::vector<SysTime>& sys_times() const { return sys_times_; }
private:
    void compile(Scheduler& sched) {
        meta_world_ = &sched.meta_world(); plan_.clear();
        auto sg = schedule::build_dag(*meta_world_);
        auto layers = graph::algo::kahn_layers(sg);
        for (const auto& layer : layers) {
            for (auto node_idx : layer) {
                entity_t e = sg.key(node_idx).entity;
                if (meta_world_->entity(e).get<schedule::SysExecutor>()) plan_.push_back(e);
            }
        }
    }
    void flush_all_buffers(World* world) { meta_world_->query<schedule::SysCmdBuf>().each([&](auto& cmd) { if (cmd.ptr && !cmd.ptr->headers().empty()) { world->submit(*cmd.ptr); cmd.ptr->clear(); } }); }
    World* meta_world_ = nullptr; std::vector<entity_t> plan_; World* bound_world_ = nullptr;
    std::vector<SysTime> sys_times_;
};

class TaskflowExecutor : public schedule::SysExecutor {
public:
    static std::unique_ptr<TaskflowExecutor> build_from(Scheduler& sched) { auto exec = std::make_unique<TaskflowExecutor>(); exec->compile(sched); return exec; }
    void init_all(World* world) {
        auto& meta = *meta_ptr_;
        meta.query<schedule::SysFactory, schedule::SysExecutor>().each([&](auto& f, auto& e) { e.func = f.func(world); });
        meta.query<schedule::SysQuery>().each([&](auto& q) { if (q.ptr) world->update_query(*static_cast<QueryState*>(q.ptr.get())); });
        meta.query<schedule::SysCmdBuf>().each([&](auto& c) {
            if (!c.ptr) c.ptr = std::make_shared<CommandBuffer>(&world->index());
            else c.ptr->set_index(&world->index());
        });
        bound_world_ = world;
    }
    void run(World* world) {
        if (world != bound_world_) init_all(world);
        current_world_ = world;
#ifdef ELYSIA_PERF_OVERLAY
        sys_times_.clear();
#endif
        executor_.run(taskflow_).wait();
        flush_all_buffers(world);
        current_world_ = nullptr;
    }
    struct SysTime { const char* name; float ms; };
    const std::vector<SysTime>& sys_times() const { return sys_times_; }
private:
    void compile(Scheduler& sched) {
        meta_ptr_ = &sched.meta_world(); auto sg = schedule::build_dag(*meta_ptr_); std::unordered_map<uint32_t, tf::Task> tasks;
        for (size_t i = 0; i < sg.node_count(); ++i) {
            const auto& gn = sg.key(i);
            tasks[i] = taskflow_.emplace([this, e = gn.entity]() {
                auto* w = current_world_; auto view = meta_ptr_->entity(e); auto* exec = view.get<schedule::SysExecutor>(); if (!exec) return;
                if (auto* q = view.get<schedule::SysQuery>()) w->update_query(*static_cast<QueryState*>(q->ptr.get()));
                auto* cp_comp = view.get<schedule::SysCmdBuf>(); void* cp = cp_comp ? cp_comp->ptr.get() : nullptr;

#ifdef ELYSIA_PERF_OVERLAY
                using Clock = std::chrono::steady_clock;
                using Ms    = std::chrono::duration<float, std::milli>;
                auto t0 = Clock::now();
#endif
                if (exec->kind == schedule::SpecialSystemKind::ApplyDeferred) {
                    flush_all_buffers(w);
                } else if (exec->func) {
                    exec->func(w, nullptr, cp);
                }

#ifdef ELYSIA_PERF_OVERLAY
                float ms = Ms(Clock::now() - t0).count();
                std::lock_guard<std::mutex> lock(times_mtx_);
                const char* name = "";
                if (auto* n = view.get<schedule::SysName>()) name = n->value.c_str();
                sys_times_.push_back({name, ms});
#endif
            }).name(std::to_string(i));
        }
        for (size_t i = 0; i < sg.node_count(); ++i) { for (const auto& edge : sg.out_edges(i)) tasks[i].precede(tasks[edge.to]); }
    }
    void flush_all_buffers(World* world) { meta_ptr_->query<schedule::SysCmdBuf>().each([&](auto& c) { if (c.ptr && !c.ptr->headers().empty()) { world->submit(*c.ptr); c.ptr->clear(); } }); }
    tf::Executor executor_; tf::Taskflow taskflow_; World* current_world_ = nullptr; World* meta_ptr_ = nullptr; World* bound_world_ = nullptr;
    std::vector<SysTime> sys_times_;
    std::mutex times_mtx_;
};

} // namespace elysia
