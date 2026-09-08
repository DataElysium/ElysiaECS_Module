module;
#include <cassert>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module elysia.query;

import elysia.config;
import elysia.meta;
import elysia.storage;
import elysia.bitset;
import elysia.core;
import elysia.entity;
import elysia.table;

export namespace elysia {

// --- Concepts ---
template <typename T>
concept ResourceType = is_res_handle_v<T>;
template <typename T>
concept EntityType = std::is_same_v<std::remove_cvref_t<T>, Entity>;
template <typename T>
concept QueryOptionArg = is_include_v<T> || is_include_inactive_v<T> ||
                         is_include_all_v<T>;
template <typename T>
concept QueryDataArg =
    !is_with_v<T> && !is_without_v<T> && !QueryOptionArg<T> && !is_res_handle_v<T> &&
    !EntityType<T> && !std::is_empty_v<std::remove_cvref_t<T>>;

template <typename T>
concept ValidQueryArg = is_with_v<T> || is_without_v<T> || QueryOptionArg<T> || ResourceType<T> ||
                        EntityType<T> || QueryDataArg<T>;

/**
 * @brief Type-erased state for an ECS Query.
 */
struct QueryState {
  SignatureBuffer<> with_mask;
  SignatureBuffer<> without_mask;
  bool is_prepared = false;
  size_t scanned_count = 0;

  std::vector<const TypeInfo *> external_withs;
  std::vector<const TypeInfo *> external_withouts;
  std::vector<Archetype<DefaultConfig> *> matched_archetypes;
  std::vector<std::vector<size_t>> column_indices;

  inline const std::vector<Archetype<DefaultConfig> *> &archetypes() const {
    return matched_archetypes;
  }

  void (*prepare_ptr)(QueryState *, ComponentRegistry &) = nullptr;
  void (*update_arch_ptr)(QueryState *, Archetype<DefaultConfig> *) = nullptr;

  inline void prepare(ComponentRegistry &reg) {
    if (prepare_ptr)
      prepare_ptr(this, reg);
  }
  inline void update_archetype(Archetype<DefaultConfig> *arch) {
    if (update_arch_ptr)
      update_arch_ptr(this, arch);
  }

  inline void add_external_filter(const TypeInfo *info, bool is_without) {
    if (is_without)
      external_withouts.push_back(info);
    else
      external_withs.push_back(info);
  }
};

// Runtime descriptors contain IDs only; no reflection or native component type is required.
struct DynamicQueryDesc {
    std::vector<uint64_t> columns;
    std::vector<uint64_t> with;
    std::vector<uint64_t> without;
    bool include_inactive = false;
};

struct DynamicColumnView {
    uint64_t id;
    void* data;
    size_t stride;
    size_t alignment;
};
struct DynamicChunkView {
    std::span<const Entity> entities;
    std::span<const DynamicColumnView> columns;
};

class DynamicQuery : public QueryState {
public:
    explicit DynamicQuery(DynamicQueryDesc descriptor = {}) : descriptor_(std::move(descriptor)) {
        for (size_t i = 0; i < descriptor_.columns.size(); ++i)
            if (std::find(descriptor_.columns.begin(), descriptor_.columns.begin() + i,
                          descriptor_.columns[i]) != descriptor_.columns.begin() + i)
                throw std::invalid_argument("Dynamic query has duplicate column id " +
                                            std::to_string(descriptor_.columns[i]));
        for (uint64_t id : descriptor_.without)
            if (mentions(descriptor_.columns, id) || mentions(descriptor_.with, id))
                throw std::invalid_argument("Dynamic query both requires and excludes component id " +
                                            std::to_string(id));
        prepare_ptr = [](QueryState* state, ComponentRegistry& registry) {
            static_cast<DynamicQuery*>(state)->prepare_impl(registry);
        };
        update_arch_ptr = [](QueryState* state, Archetype<DefaultConfig>* arch) {
            static_cast<DynamicQuery*>(state)->update_archetype_impl(arch);
        };
    }

    const DynamicQueryDesc& descriptor() const { return descriptor_; }
    // Also use reset after moving/replacing the world, or changing inherited external filters.
    void reset() {
        is_prepared = false;
        registry_ = nullptr;
        scanned_count = 0;
        matched_archetypes.clear();
        column_indices.clear();
        column_types_.clear();
        with_mask = SignatureBuffer<>{};
        without_mask = SignatureBuffer<>{};
    }

    // Call world.update_query(query) before iteration. Views are borrowed for the callback.
    // Structural mutation during iteration is unsupported, just as for typed queries.
    void each_chunk(void (*visitor)(void*, const DynamicChunkView&), void* context) const {
        if (!visitor) throw std::invalid_argument("Null dynamic query visitor");
        if (!is_prepared) throw std::logic_error("Dynamic query must be updated before iteration");
        std::vector<DynamicColumnView> columns(column_types_.size());
        for (size_t a = 0; a < matched_archetypes.size(); ++a) {
            for (const auto& chunk : matched_archetypes[a]->table().chunks()) {
                if (chunk->count() == 0) continue;
                for (size_t c = 0; c < columns.size(); ++c)
                    columns[c] = {column_types_[c]->id,
                                  chunk->component(column_indices[a][c], 0),
                                  column_types_[c]->size, column_types_[c]->alignment};
                DynamicChunkView view{{&chunk->entity(0), chunk->count()}, columns};
                visitor(context, view);
            }
        }
    }
    template <class Func> void each_chunk(Func&& visitor) const {
        // A local adapter also supports const callable objects and free functions.
        auto adapter = [&](const DynamicChunkView& view) { visitor(view); };
        each_chunk([](void* context, const DynamicChunkView& view) {
            (*static_cast<decltype(adapter)*>(context))(view);
        }, &adapter);
    }

private:
    static bool mentions(const std::vector<uint64_t>& ids, uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
    void prepare_impl(ComponentRegistry& registry) {
        if (is_prepared && registry_ == &registry) return;
        reset();
        auto require = [&](uint64_t id) -> const TypeInfo* {
            auto* info = registry.get_info(id);
            if (!info)
                throw std::invalid_argument("Dynamic query uses unregistered component id " + std::to_string(id));
            return info;
        };
        for (uint64_t id : descriptor_.columns) {
            auto* info = require(id);
            if (info->size == 0)
                throw std::invalid_argument("Dynamic query tag must be a filter, not a data column: " +
                                            std::to_string(id));
            column_types_.push_back(info);
            with_mask.set(registry.local_index(id));
        }
        for (uint64_t id : descriptor_.with) {
            require(id);
            with_mask.set(registry.local_index(id));
        }
        for (uint64_t id : descriptor_.without) {
            require(id);
            without_mask.set(registry.local_index(id));
        }
        bool disabled_mentioned = mentions(descriptor_.columns, TypeTraits<DisabledTag>::id) ||
                                  mentions(descriptor_.with, TypeTraits<DisabledTag>::id) ||
                                  mentions(descriptor_.without, TypeTraits<DisabledTag>::id);
        for (const auto* info : external_withs) {
            if (!info) throw std::invalid_argument("Null dynamic query filter");
            with_mask.set(registry.ensure_registered(info));
            disabled_mentioned |= info->id == TypeTraits<DisabledTag>::id;
        }
        for (const auto* info : external_withouts) {
            if (!info) throw std::invalid_argument("Null dynamic query filter");
            without_mask.set(registry.ensure_registered(info));
            disabled_mentioned |= info->id == TypeTraits<DisabledTag>::id;
        }
        if (!descriptor_.include_inactive && !disabled_mentioned)
            without_mask.set(registry.ensure_registered(get_type_info_ptr<DisabledTag>()));
        registry_ = &registry;
        is_prepared = true;
    }
    void update_archetype_impl(Archetype<DefaultConfig>* arch) {
        if (!arch || !match_all(arch->bit_sig(), with_mask) || intersects(arch->bit_sig(), without_mask))
            return;
        if (std::find(matched_archetypes.begin(), matched_archetypes.end(), arch) != matched_archetypes.end())
            return;
        std::vector<size_t> indices;
        indices.reserve(descriptor_.columns.size());
        for (uint64_t id : descriptor_.columns) {
            auto column = arch->get_column_index(id);
            if (!column) return;
            indices.push_back(*column);
        }
        column_indices.push_back(std::move(indices));
        try {
            matched_archetypes.push_back(arch);
        } catch (...) {
            column_indices.pop_back();
            throw;
        }
    }

    DynamicQueryDesc descriptor_;
    const ComponentRegistry* registry_ = nullptr;
    std::vector<const TypeInfo*> column_types_;
};

template <typename T> struct get_res_type {
  using type = std::tuple<>;
};
template <typename T>
  requires ResourceType<T>
struct get_res_type<T> {
  using type = std::tuple<typename std::remove_cvref_t<T>::type>;
};

template <ValidQueryArg... Args> class Query : public QueryState {
public:
  using DataTuple = decltype(std::tuple_cat(
      std::conditional_t<QueryDataArg<Args>,
                         std::tuple<std::remove_cvref_t<Args>>,
                         std::tuple<>>{}...));
  using ResTuple =
      decltype(std::tuple_cat(typename get_res_type<Args>::type{}...));

  static constexpr size_t DataCount = std::tuple_size_v<DataTuple>;
  static constexpr size_t ResCount = std::tuple_size_v<ResTuple>;

  Query() {
    static_assert(is_unique_v<GetRawT<Args>...>,
                  "Elysia Error: Query contains duplicate component types!");
    prepare_ptr = [](QueryState *self, ComponentRegistry &reg) {
      static_cast<Query *>(self)->prepare_impl(reg);
    };
    update_arch_ptr = [](QueryState *self, Archetype<DefaultConfig> *arch) {
      static_cast<Query *>(self)->update_archetype_impl(arch);
    };
  }

  // ⚠️ THREAD-SAFETY: prepare_impl() writes ComponentRegistry (ensure_registered)
  // and internal vectors (matched_archetypes, column_indices). When called
  // concurrently from parallel systems via update_query(), two systems with
  // different unregistered component types may race on next_local_index_++
  // and unordered_map insert.
  // 👉 CONSTRAINT: System types are registered during Scheduler build-phase.
  //    Runtime system insertion must go through single-threaded cmd path.
  // 👉 FUTURE: Move query pre-prepare to Executor::init_all() phase to
  //    guarantee is_prepared==true before any parallel wave executes.
  void prepare_impl(ComponentRegistry &reg) {
    if (is_prepared)
      return;
    with_mask = SignatureBuffer<>{};
    without_mask = SignatureBuffer<>{};
    constexpr bool include_all = (is_include_all_v<Args> || ...);
    constexpr bool include_inactive = (is_include_inactive_v<Args> || ...);
    (
        [&]<typename T>(std::type_identity<T>) {
          if constexpr (ResourceType<T>)
            return;
          if constexpr (EntityType<T>)
            return;
          if constexpr (QueryOptionArg<T>)
            return;
          uint32_t id = reg.ensure_registered(get_type_info_ptr<GetRawT<T>>());
          if constexpr (is_without_v<T>)
            without_mask.set(id);
          else
            with_mask.set(id);
        }(std::type_identity<Args>{}),
        ...);
    for (const auto *info : external_withs)
      with_mask.set(reg.ensure_registered(info));
    for (const auto *info : external_withouts)
      without_mask.set(reg.ensure_registered(info));
    auto external_mentions = [&](uint64_t id) {
      for (const auto *info : external_withs)
        if (info && info->id == id)
          return true;
      for (const auto *info : external_withouts)
        if (info && info->id == id)
          return true;
      return false;
    };

    auto add_default_without = [&]<typename T>() {
      constexpr bool mentioned = (std::is_same_v<GetRawT<Args>, T> || ...);
      if constexpr (!include_all && !include_inactive && !mentioned) {
        if (!external_mentions(TypeTraits<T>::id))
          without_mask.set(reg.ensure_registered(get_type_info_ptr<T>()));
      }
    };
    add_default_without.template operator()<DisabledTag>();

    is_prepared = true;
  }

  void update_archetype_impl(Archetype<DefaultConfig> *arch) {
    if (!arch) return;
    if (!match_all(arch->bit_sig(), with_mask))
      return;
    if (intersects(arch->bit_sig(), without_mask))
      return;
    
    for (auto* existing : matched_archetypes) if (existing == arch) return;

    std::vector<size_t> indices;
    indices.reserve(DataCount);
    bool all_found = true;
    if constexpr (DataCount > 0)
      parse_indices<DataTuple>(arch, indices, all_found,
                               std::make_index_sequence<DataCount>{});

    if (all_found) {
      matched_archetypes.push_back(arch);
      column_indices.push_back(std::move(indices));
    }
  }

  struct ChunkView {
    Chunk *chunk;
    const size_t *cols;
    size_t count;

    template <typename Func> void iter(Func &&func) const {
      [&]<size_t... Is>(std::index_sequence<Is...>) {
        func(count, static_cast<std::tuple_element_t<Is, DataTuple> *>(
                        chunk->component(cols[Is], 0))...);
      }(std::make_index_sequence<DataCount>{});
    }
  };

  struct ChunkIterator {
    Query *q;
    size_t arch_idx;
    size_t chunk_idx;

    ChunkIterator &operator++() {
      chunk_idx++;
      seek_next();
      return *this;
    }

    bool operator==(const ChunkIterator &other) const {
      return arch_idx == other.arch_idx && chunk_idx == other.chunk_idx;
    }
    bool operator!=(const ChunkIterator &other) const {
      return !(*this == other);
    }

    ChunkView operator*() const {
      auto *arch = q->matched_archetypes[arch_idx];
      auto &ck = arch->table().chunks()[chunk_idx];
      return {ck.get(), q->column_indices[arch_idx].data(), ck->count()};
    }

    void seek_next() {
      while (arch_idx < q->matched_archetypes.size()) {
        auto *arch = q->matched_archetypes[arch_idx];
        auto &chunks = arch->table().chunks();
        while (chunk_idx < chunks.size()) {
          if (chunks[chunk_idx]->count() > 0)
            return;
          chunk_idx++;
        }
        arch_idx++;
        chunk_idx = 0;
      }
    }
  };

  struct ChunkRange {
    Query *q;
    ChunkIterator begin() {
      ChunkIterator it{q, 0, 0};
      it.seek_next();
      return it;
    }
    ChunkIterator end() { return {q, q->matched_archetypes.size(), 0}; }
    /// return all entities across tables matched by this query (slow)
    size_t size() const {
      size_t total = 0;
      for (auto *arch : q->matched_archetypes)
        total += arch->table().count();
      return total;
    }
  };
  bool empty() const {
    if (matched_archetypes.empty())
      return true;
    for (auto *arch : matched_archetypes) {
      if (arch->table().count() > 0)
        return false;
    }
    return true;
  }
  ChunkRange chunks() { return {this}; }
  /// return all entities across tables matched by this query (slow)
  size_t size() { return ChunkRange{this}.size(); }
  template <typename Func, typename WorldPtr>
  void iter(WorldPtr *world, Func &&func) {
    auto resources =
        resolve_resources(world, std::make_index_sequence<ResCount>{});
    for (auto cv : chunks()) {
      cv.iter([&](size_t count, auto *...ptrs) {
        [&]<size_t... Js>(std::index_sequence<Js...>) {
          auto res_instances =
              std::make_tuple((Res<std::tuple_element_t<Js, ResTuple>>{
                  *std::get<Js>(resources)})...);
          func(count, ptrs..., std::get<Js>(res_instances)...);
        }(std::make_index_sequence<ResCount>{});
      });
    }
  }

  template <typename Func, typename WorldPtr>
  void each(WorldPtr *world, Func &&func) {
    auto resources =
        resolve_resources(world, std::make_index_sequence<ResCount>{});
    for (auto cv : chunks()) {
      [&]<size_t... Is>(std::index_sequence<Is...>) {
        auto ptrs =
            std::make_tuple(static_cast<std::tuple_element_t<Is, DataTuple> *>(
                cv.chunk->component(cv.cols[Is], 0))...);
        for (size_t i = 0; i < cv.count; ++i) {
          invoke_final(func, cv.chunk, i, ptrs, resources,
                       std::make_index_sequence<sizeof...(Args)>{});
        }
      }(std::make_index_sequence<DataCount>{});
    }
  }

  template <typename Func> void each(Func &&func) {
    each(static_cast<void *>(nullptr), std::forward<Func>(func));
  }
  template <typename Func> void iter(Func &&func) {
    iter(static_cast<void *>(nullptr), std::forward<Func>(func));
  }

private:
  template <typename Tuple, size_t... Is>
  void parse_indices(Archetype<DefaultConfig> *arch, std::vector<size_t> &out,
                     bool &ok, std::index_sequence<Is...>) {
    ((ok = ok &&
           [&]() {
             auto idx = arch->get_column_index(
                 TypeTraits<std::tuple_element_t<Is, Tuple>>::id);
             if (idx)
               out.push_back(*idx);
             return idx.has_value();
           }()),
     ...);
  }

  template <size_t... Is, typename WorldPtr>
  auto resolve_resources(WorldPtr *world, std::index_sequence<Is...>) {
    if constexpr (ResCount > 0)
      return std::make_tuple(
          detail::checked_resource(world->template get_resource<std::tuple_element_t<Is, ResTuple>>())...);
    else
      return std::make_tuple();
  }

  template <typename Func, typename DPtrs, typename RPtrs, size_t... Is>
  inline void invoke_final(Func &func, auto *chunk, size_t row, DPtrs &d_ptrs,
                           RPtrs &r_ptrs, std::index_sequence<Is...>) {
    auto res_instances = std::make_tuple(([&]() {
      using T = std::tuple_element_t<Is, std::tuple<Args...>>;
      if constexpr (ResourceType<T>) {
        constexpr size_t idx = count_res_before<Is>();
        auto *ptr = std::get<idx>(r_ptrs);
        return std::make_tuple(std::remove_cvref_t<T>{*ptr});
      } else
        return std::tuple<>{};
    }())...);
    auto final_refs = std::tuple_cat(([&]() {
      using T = std::tuple_element_t<Is, std::tuple<Args...>>;
      if constexpr (ResourceType<T>)
        return std::forward_as_tuple(std::get<0>(std::get<Is>(res_instances)));
      else if constexpr (EntityType<T>)
        return std::forward_as_tuple(chunk->entity(row));
      else if constexpr (QueryDataArg<T>)
        return std::forward_as_tuple(
            (std::get<count_data_before<Is>()>(d_ptrs))[row]);
      else
        return std::tuple<>{};
    }())...);
    std::apply(func, final_refs);
  }

  // Named predicates to avoid lambda ODR issues in modules
  struct QueryDataPredicate {
    template <typename T> static constexpr bool check(T) {
      return QueryDataArg<typename decltype(T{})::type>;
    }
  };
  struct ResourcePredicate {
    template <typename T> static constexpr bool check(T) {
      return ResourceType<typename decltype(T{})::type>;
    }
  };

  template <size_t N> static constexpr size_t count_data_before() {
    return count_impl<N, QueryDataPredicate>(std::make_index_sequence<N>{});
  }
  template <size_t N> static constexpr size_t count_res_before() {
    return count_impl<N, ResourcePredicate>(std::make_index_sequence<N>{});
  }
  template <size_t N, typename Pred, size_t... Is>
  static constexpr size_t count_impl(std::index_sequence<Is...>) {
    return (
        (size_t)(Pred::check(std::type_identity<
                             std::tuple_element_t<Is, std::tuple<Args...>>>{})
                     ? 1
                     : 0) +
        ... + 0);
  }
};

template <typename Func> auto make_query(Func &&) {
  using Args = args_tuple_t<std::remove_reference_t<Func>>;
  return []<typename... Ts>(std::type_identity<std::tuple<Ts...>>) {
    return Query<Ts...>{};
  }(std::type_identity<Args>{});
}

} // namespace elysia
