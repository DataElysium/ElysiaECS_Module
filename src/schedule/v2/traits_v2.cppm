module;
#include <type_traits>
#include <tuple>
#include <cstddef>
#include <cassert>
#include <functional>
#include <memory>

export module elysia.schedule:traits;

import elysia.world;
import elysia.meta;
import elysia.storage; 
import elysia.config;
import elysia.query;
import elysia.result;
import elysia.schedule.components;

export namespace elysia::schedule {

using namespace elysia;

template<typename F> using args_t = args_tuple_t<F>;
template<typename T> concept IsCommandBuffer = std::is_same_v<std::remove_cvref_t<T>, CommandBuffer> || std::is_same_v<std::remove_cvref_t<std::remove_pointer_t<std::remove_cvref_t<T>>>, CommandBuffer>;

template<typename Tuple> struct filter_cmd_buf_impl;
template<typename... Args>
struct filter_cmd_buf_impl<std::tuple<Args...>> {
    using type = decltype(std::tuple_cat(std::declval<std::conditional_t<IsCommandBuffer<Args>, std::tuple<>, std::tuple<Args>>>()...));
};

template<typename Tuple> struct filter_global_args_impl;
template<typename... Args>
struct filter_global_args_impl<std::tuple<Args...>> {
    using type = decltype(std::tuple_cat(std::declval<std::conditional_t<IsCommandBuffer<Args> || is_res_handle_v<Args>, std::tuple<>, std::tuple<Args>>>()...));
};

template<typename F> using query_args_t = typename filter_cmd_buf_impl<args_t<F>>::type;
template<typename F> using non_global_args_t = typename filter_global_args_impl<args_t<F>>::type;

template<typename... Args> constexpr bool check_has_cmd(std::tuple<Args...>*) { return (IsCommandBuffer<Args> || ...); }
template<typename F> constexpr bool has_cmd_buf_v = check_has_cmd((args_t<F>*)nullptr);

template<typename F> concept IsWorldSystem = requires { typename args_t<F>; requires std::tuple_size_v<args_t<F>> == 1; requires std::is_convertible_v<std::tuple_element_t<0, args_t<F>>, const World*>; };
template<typename F> concept IsWorldMutSystem = IsWorldSystem<F> && std::is_convertible_v<std::tuple_element_t<0, args_t<F>>, World*>;
template<typename F> concept IsCmdSystem = requires { typename args_t<F>; requires std::tuple_size_v<args_t<F>> == 1; requires IsCommandBuffer<std::tuple_element_t<0, args_t<F>>>; };
template<typename F> concept IsGlobalSystem = !IsCmdSystem<F> && (std::tuple_size_v<non_global_args_t<F>> == 0);
template<typename F> concept IsIterSystem = requires { typename args_t<F>; requires std::tuple_size_v<args_t<F>> == 1; requires std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0, args_t<F>>>, Iter>; };
template<typename F> concept IsWorldCmdSystem = !IsIterSystem<F> && !IsWorldSystem<F> && !IsCmdSystem<F> && !IsGlobalSystem<F> && requires(F& instance, World* w, CommandBuffer* c) { instance(w, c); };
template<typename F> concept IsBulkSystem = requires { typename query_args_t<F>; requires std::tuple_size_v<query_args_t<F>> >= 1; requires std::is_convertible_v<std::tuple_element_t<0, query_args_t<F>>, size_t>; };

// ── Short-named concept aliases for partial specialization dispatch ──
template<typename F> concept IterSystem     = IsIterSystem<F>;
template<typename F> concept WorldCmdSystem = IsWorldCmdSystem<F>;
template<typename F> concept WorldMutSystem = IsWorldMutSystem<F>;
template<typename F> concept WorldReadSystem = IsWorldSystem<F> && !IsWorldMutSystem<F>;
template<typename F> concept CmdSystem      = IsCmdSystem<F>;
template<typename F> concept GlobalSystem   = IsGlobalSystem<F>;
template<typename F> concept BulkSystem     = IsBulkSystem<F>;

// ── WorldView-based system concepts (parallel-safe) ──────────────────────────
template<typename F> concept WorldViewSystem =
    requires(F f, WorldView v) { f(v); };
template<typename F> concept WorldViewCmdSystem =
    !WorldViewSystem<F> &&
    requires(F f, WorldView v, CommandBuffer* c) { f(v, c); };

template<typename T> struct strip_pointer { using type = T; };
template<typename T> struct strip_pointer<T*> { using type = T; };
template<typename T> struct strip_pointer<const T*> { using type = const T; };

template<typename Tuple> struct bulk_args_impl;
template<typename SizeT, typename... Rest>
struct bulk_args_impl<std::tuple<SizeT, Rest...>> { using type = std::tuple<typename strip_pointer<Rest>::type...>; };

template<typename F> using bulk_query_args_stripped_t = typename bulk_args_impl<query_args_t<F>>::type;

// =============================================================================
// Query type helper
// =============================================================================
template <typename T> struct tuple_to_query;
template <typename... Ts> struct tuple_to_query<std::tuple<Ts...>> {
  using type = Query<Ts...>;
};

// =============================================================================
// invoke_wrapper — normalizes void vs Result<void> return
// =============================================================================
template <typename F>
auto make_invoke_wrapper(F&& f) {
  return [f = std::forward<F>(f)](auto&&... args) mutable -> Result<void> {
    using ResultType =
        std::invoke_result_t<std::decay_t<F>, decltype(args)...>;
    if constexpr (std::is_void_v<ResultType>) {
      f(std::forward<decltype(args)>(args)...);
      return Result<void>::ok();
    } else {
      return f(std::forward<decltype(args)>(args)...);
    }
  };
}

using SysExecutorFn = std::function<Result<void>(World*, void*, void*)>;

// =============================================================================
// System Adapters — one per system kind
// Each adapter implements:
//   static std::shared_ptr<void> make_query()
//   static SysExecutorFn      make_executor(F&& f, std::shared_ptr<void>& q)
// =============================================================================

struct IterSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() { return nullptr; }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& /*q*/) {
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [invoke = std::move(invoke)](World* w, void* q_void,
                                        void* c_void) mutable -> Result<void> {
      Iter it(w, static_cast<QueryState*>(q_void),
              static_cast<CommandBuffer*>(c_void));
      return invoke(it);
    };
  }
};

struct WorldCmdSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() { return nullptr; }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& /*q*/) {
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [invoke = std::move(invoke)](World* w, void*,
                                        void* c_void) mutable -> Result<void> {
      assert(c_void != nullptr && "Elysia Error: CommandBuffer is null in WorldCmdSystemAdapter!");
      return invoke(w, static_cast<CommandBuffer*>(c_void));
    };
  }
};

struct WorldSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() { return nullptr; }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& /*q*/) {
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [invoke = std::move(invoke)](World* w, void*,
                                        void*) mutable -> Result<void> {
      return invoke(w);
    };
  }
};

struct CmdSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() { return nullptr; }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& /*q*/) {
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [invoke = std::move(invoke)](World*, void*,
                                        void* c_void) mutable -> Result<void> {
      assert(c_void != nullptr && "Elysia Error: CommandBuffer is null in CmdSystemAdapter!");
      return invoke(*static_cast<CommandBuffer*>(c_void));
    };
  }
};

struct GlobalSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() { return nullptr; }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& /*q*/) {
    using RawFunc = std::decay_t<F>;
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return
        [invoke = std::move(invoke)](World* w, void*,
                                     void* c_void) mutable -> Result<void> {
          if constexpr (has_cmd_buf_v<RawFunc>) {
            assert(c_void != nullptr && "Elysia Error: CommandBuffer is null in GlobalSystemAdapter!");
          }
          using Args = args_t<RawFunc>;
          auto run_with_args =
              [&]<size_t... Is>(std::index_sequence<Is...>) {
                auto args_tuple =
                    std::make_tuple(([&]() -> decltype(auto) {
                      using T = std::tuple_element_t<Is, Args>;
                      using DecayedT = std::decay_t<T>;
                      if constexpr (IsCommandBuffer<DecayedT>) {
                        if constexpr (std::is_pointer_v<DecayedT>) {
                          return static_cast<CommandBuffer*>(c_void);
                        } else {
                          return *static_cast<CommandBuffer*>(c_void);
                        }
                      }
                      else if constexpr (is_res_handle_v<DecayedT>) {
                        auto* ptr =
                            w->get_resource<typename DecayedT::type>();
                        assert(ptr &&
                               "Elysia Error: Requested Resource Not Found "
                               "in World!");
                        return DecayedT{*ptr};
                      }
                    })()...);
                return invoke(std::get<Is>(args_tuple)...);
              };
          return run_with_args(
              std::make_index_sequence<std::tuple_size_v<Args>>{});
        };
  }
};

struct BulkSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() {
    using RawFunc = std::decay_t<F>;
    return std::make_shared<
        typename tuple_to_query<bulk_query_args_stripped_t<RawFunc>>::type>();
  }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& query) {
    using RawFunc = std::decay_t<F>;
    constexpr bool has_cmd = has_cmd_buf_v<RawFunc>;
    using Q = typename tuple_to_query<bulk_query_args_stripped_t<RawFunc>>::type;
    auto q = std::static_pointer_cast<Q>(query);
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [q, invoke = std::move(invoke)](
               World* w, void*, void* c_void) mutable -> Result<void> {
      w->update_query(*q);
      if constexpr (has_cmd) {
        assert(c_void != nullptr && "Elysia Error: CommandBuffer is null in BulkSystemAdapter!");
      }
      q->iter(w, [&](auto&&... args) {
        // ⚠️ TODO(CAUTION): has_cmd detects CommandBuffer in signature but
        // SysCmdBuf is only injected for takes_wv_cmd/takes_world_cmd systems.
        // If a system has CommandBuffer param but no SysCmdBuf injection,
        // c_void is nullptr → *static_cast<CommandBuffer*>(c_void) is UB.
        if constexpr (has_cmd)
          invoke(*static_cast<CommandBuffer*>(c_void),
                 std::forward<decltype(args)>(args)...);
        else
          invoke(std::forward<decltype(args)>(args)...);
      });
      return Result<void>::ok();
    };
  }
};

struct EachSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() {
    using RawFunc = std::decay_t<F>;
    return std::make_shared<
        typename tuple_to_query<query_args_t<RawFunc>>::type>();
  }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& query) {
    using RawFunc = std::decay_t<F>;
    constexpr bool has_cmd = has_cmd_buf_v<RawFunc>;
    using Q = typename tuple_to_query<query_args_t<RawFunc>>::type;
    auto q = std::static_pointer_cast<Q>(query);
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [q, invoke = std::move(invoke)](
               World* w, void*, void* c_void) mutable -> Result<void> {
      w->update_query(*q);
      if constexpr (has_cmd) {
        assert(c_void != nullptr && "Elysia Error: CommandBuffer is null in EachSystemAdapter!");
      }
      q->each(w, [&](auto&&... args) {
        // ⚠️ TODO(CAUTION): has_cmd detects CommandBuffer in signature but
        // SysCmdBuf is only injected for takes_wv_cmd/takes_world_cmd systems.
        // If a system has CommandBuffer param but no SysCmdBuf injection,
        // c_void is nullptr → *static_cast<CommandBuffer*>(c_void) is UB.
        if constexpr (has_cmd)
          invoke(*static_cast<CommandBuffer*>(c_void),
                 std::forward<decltype(args)>(args)...);
        else
          invoke(std::forward<decltype(args)>(args)...);
      });
      return Result<void>::ok();
    };
  }
};

struct WorldViewSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() { return nullptr; }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& /*q*/) {
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [invoke = std::move(invoke)](World* w, void*, void*) mutable -> Result<void> {
      return invoke(WorldView{w});
    };
  }
};

struct WorldViewCmdSystemAdapter {
  template <typename F>
  static std::shared_ptr<void> make_query() { return nullptr; }

  template <typename F>
  static SysExecutorFn make_executor(F&& f, std::shared_ptr<void>& /*q*/) {
    auto invoke = make_invoke_wrapper(std::forward<F>(f));
    return [invoke = std::move(invoke)](World* w, void*, void* c_void) mutable -> Result<void> {
      assert(c_void != nullptr && "Elysia Error: CommandBuffer is null in WorldViewCmdSystemAdapter!");
      return invoke(WorldView{w}, static_cast<CommandBuffer*>(c_void));
    };
  }
};

// =============================================================================
// Priority-based Adapter Selector — flat partial specializations
// =============================================================================
template <typename F>
struct system_adapter_selector { using type = EachSystemAdapter; };

template <IterSystem F>
struct system_adapter_selector<F> { using type = IterSystemAdapter; };

template <WorldViewCmdSystem F>
struct system_adapter_selector<F> { using type = WorldViewCmdSystemAdapter; };

template <WorldViewSystem F>
struct system_adapter_selector<F> { using type = WorldViewSystemAdapter; };

template <WorldCmdSystem F>
struct system_adapter_selector<F> { using type = WorldCmdSystemAdapter; };

template <WorldMutSystem F>
struct system_adapter_selector<F> { using type = WorldSystemAdapter; };

template <WorldReadSystem F>
struct system_adapter_selector<F> { using type = WorldSystemAdapter; };

template <CmdSystem F>
struct system_adapter_selector<F> { using type = CmdSystemAdapter; };

template <GlobalSystem F>
struct system_adapter_selector<F> { using type = GlobalSystemAdapter; };

template <BulkSystem F>
struct system_adapter_selector<F> { using type = BulkSystemAdapter; };

template <typename F>
using system_adapter_t = typename system_adapter_selector<F>::type;

} // namespace elysia::schedule
