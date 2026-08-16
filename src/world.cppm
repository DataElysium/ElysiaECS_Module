module;
#include <cassert>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
export module elysia.world:world;
import :fwd;
import :command;

import elysia.meta;
import elysia.result;
import elysia.core;
import elysia.storage;
import elysia.graph;
import elysia.query;
import elysia.entity;
import elysia.table;
import elysia.config;
import elysia.observer;

export namespace elysia {

class ELYSIA_API World;
struct ELYSIA_API WorldView;

/**
 * @brief AutoQuery — a Query that automatically calls update_query before
 * every each/iter/chunks invocation. Designed as a struct member in functor
 * systems so users never need to write w->update_query(q) / w.update_query(q)
 * manually. Works with both World* (legacy) and WorldView (current) functors.
 *
 * Usage: replace `Query<A, B>` with `AutoQuery<A, B>` in your functor struct.
 */
template <ValidQueryArg... Args> class AutoQuery : public Query<Args...> {
public:
  // World* overloads
  template <typename Func> void each(World *w, Func &&func);
  template <typename Func> void iter(World *w, Func &&func);
  auto chunks(World *w);

  // WorldView overloads
  template <typename Func> void each(WorldView w, Func &&func);
  template <typename Func> void iter(WorldView w, Func &&func);
  auto chunks(WorldView w);

  using Query<Args...>::each;
  using Query<Args...>::iter;
  using Query<Args...>::chunks;
};

template <ValidQueryArg... Args> class BoundQuery {
public:
  BoundQuery(World *w) : world_(w) {}

  template <typename... FilterArgs> BoundQuery &filter() {
    (
        [&]() {
          if constexpr (is_without_v<FilterArgs>) {
            query_.add_external_filter(
                get_type_info_ptr<typename FilterArgs::type>(), true);
          } else if constexpr (is_with_v<FilterArgs>) {
            query_.add_external_filter(
                get_type_info_ptr<typename FilterArgs::type>(), false);
          } else {
            query_.add_external_filter(get_type_info_ptr<FilterArgs>(), false);
          }
        }(),
        ...);
    return *this;
  }

  template <typename Func> void each(Func &&func);

  template <typename Func> void iter(Func &&func);

private:
  World *world_;
  Query<Args...> query_;
};

// =============================================================================
// [Accessors]
// ==============================================================================

struct ResourceAccessor {
  World *world;
  template <typename T> inline void add(T &&res);
  template <typename T> inline T *get() const;
  /**
   * @brief Ensures a resource exists. Creates one with default constructor if
   * missing.
   */
  template <typename T> T *get_or_create();
};

struct CommandAccessor {
  World *world;
  void submit(CommandBuffer &cmd);
  template <typename T>
  inline void decorate(
      std::function<void(const World &, CommandBuffer &, Entity, void *)> d);
  inline CommandBuffer &buffer_a();
  inline CommandBuffer &buffer_b();
};

struct EntityView {
  World *world;
  Entity entity;
  // rvalue / explicit-move overload — transfers ownership into ECS
  template <typename T>
    requires(!std::is_lvalue_reference_v<T>)
  inline EntityView &add(T &&component);
  // lvalue overload for copy-constructible types — copies into ECS, source
  // preserved
  template <typename T>
    requires std::copy_constructible<std::remove_cvref_t<T>>
  inline EntityView &add(const T &component);
  template <typename T> inline EntityView &remove();
  template <typename T> inline T *get();

  inline void despawn();
  Archetype<DefaultConfig> *archetype();
};

struct WorldBatchAccessor {
  World *world;
  void spawn_bundle(Entity e, std::span<const TypeInfo *const> types,
                    std::span<void *const> datas);
};

// =============================================================================
// [World]
// =============================================================================

class ELYSIA_API World {
public:
  explicit World(Allocator *alloc = nullptr);
  ~World();

  World(World &&) noexcept;
  World &operator=(World &&) noexcept;

  ResourceAccessor resources() { return {this}; }
  CommandAccessor commands() { return {this}; }
  WorldBatchAccessor batch() { return {this}; }

  template <typename... Args> auto query() { return BoundQuery<Args...>{this}; }

  //   THREAD-SAFETY: Called from ForkUnionExecutor::execute_system() in
  // parallel wave context (for_n_dynamic). prepare() may write
  // ComponentRegistry on first call. scanned_count write is non-atomic (same
  // QueryState is exclusive to a single system, safe in practice).
  //   CONSTRAINT: Each system owns a unique QueryState (via SysQuery).
  //    Types are pre-registered in build phase → prepare is no-op at runtime.
  //  FUTURE: Add init_all pre-prepare to guarantee is_prepared==true.
  void update_query(QueryState &q) {
    q.prepare(graph_.registry());
    size_t current_total = graph_.archetype_count();
    size_t scanned = q.scanned_count;
    if (scanned < current_total) {
      for (size_t i = scanned; i < current_total; ++i) {
        auto *arch = graph_.get_archetype(i);
        assert(arch != nullptr && "Graph returned null archetype!");
        q.update_archetype(arch);
      }
      q.scanned_count = current_total;
    }
  }

  /**
   * @brief Immediate spawn of a new entity.
   * @return An EntityView handle.
   * @note Use this for single-threaded initialization or exclusive systems.
   */
  EntityView spawn();

  /**
   * @brief Immediate destruction of an entity.
   * @param e The entity handle (checked for version mismatch).
   */
  void despawn(Entity e);
  void batch_despawn(std::span<const Entity> entities);
  size_t drop_archetypes_with(uint64_t type_id);
  template <typename T> size_t drop_archetypes_with() {
    return drop_archetypes_with(TypeTraits<std::remove_cvref_t<T>>::id);
  }

  /**
   * @brief Force destruction of an entity by its raw ID, ignoring version.
   * @param id The 32-bit entity ID.
   */
  void despawn(uint32_t id);

  /**
   * @brief Bulk import a range of entity IDs.
   */
  void import_entities(uint32_t start, uint32_t count);
  EntityView entity(Entity e) { return {this, e}; }

  void submit(CommandBuffer &cmd);

  // Internal exports
  Result<Entity> spawn_at(Entity e);
  void add_component_dynamic(Entity e, const TypeInfo *info, void *data);
  void remove_component_dynamic(Entity e, uint64_t type_id);

  template <typename T> T *get_component(Entity e);
  template <typename T> void add_resource(T &&res);
  template <typename T> T *get_resource() const;

  void *get_resource_dynamic(uint64_t id) const;

  ArchetypeGraph<DefaultConfig> &graph() { return graph_; }
  EntityIndex &index() { return index_; }
  size_t archetype_count() const { return graph_.archetype_count(); }

  ObserverRegistry &observer() { return observer_registry_; }

  struct ELYSIA_API ResourceRecord {
    void *ptr;
    void (*deleter)(void *);

    // 默认构造
    ResourceRecord() = default;

    // 构造函数：存入指针和对应的强类型删除器
    ResourceRecord(void *p, void (*d)(void *)) : ptr(p), deleter(d) {}

    // 禁用拷贝：防止资源被多次释
    ResourceRecord(const ResourceRecord &) = delete;
    ResourceRecord &operator=(const ResourceRecord &) = delete;

    // 支持移动：让 Map 扩容时能安全转移资源
    ResourceRecord(ResourceRecord &&other) noexcept
        : ptr(std::exchange(other.ptr, nullptr)),
          deleter(std::exchange(other.deleter, nullptr)) {}

    ResourceRecord &operator=(ResourceRecord &&other) noexcept {
      std::swap(ptr, other.ptr);
      std::swap(deleter, other.deleter);
      return *this;
    }
    ~ResourceRecord() {
      if (ptr && deleter) {
        deleter(ptr);
      }
    }
  };
  using CommandDecorator =
      std::function<void(const World &, CommandBuffer &, Entity, void *)>;

  friend void execute_buffer_internal(World &world, CommandBuffer &input,
                                      CommandBuffer &output);

private:
  friend struct ResourceAccessor;
  friend struct CommandAccessor;
  friend struct EntityView;
  friend struct WorldBatchAccessor;

  EntityIndex index_;
  ArchetypeGraph<DefaultConfig> graph_;
  ObserverRegistry observer_registry_;
  std::unordered_map<uint64_t, CommandDecorator> decorators_;
  std::unordered_map<uint64_t, ResourceRecord> resources_;

  std::shared_ptr<CommandBuffer> buffer_a_;
  std::shared_ptr<CommandBuffer> buffer_b_;
};

// --- Inlines ---

inline CommandBuffer &CommandAccessor::buffer_a() {
  return *(world->buffer_a_);
}
inline CommandBuffer &CommandAccessor::buffer_b() {
  return *(world->buffer_b_);
}
template <typename T>
inline void CommandAccessor::decorate(
    std::function<void(const World &, CommandBuffer &, Entity, void *)> d) {
  world->decorators_[TypeTraits<T>::id] = std::move(d);
}

template <typename T> inline void ResourceAccessor::add(T &&res) {
  world->add_resource<T>(std::forward<T>(res));
}
template <typename T> inline T *ResourceAccessor::get() const {
  return world->get_resource<T>();
}

template <typename T> inline T *ResourceAccessor::get_or_create() {
  auto *res = get<T>();
  if (res)
    return res;

  using RawT = std::remove_cvref_t<T>;
  auto *raw_ptr = new RawT();
  uint64_t id = TypeTraits<RawT>::id;
  world->resources_[id] = {raw_ptr,
                           [](void *ptr) { delete static_cast<RawT *>(ptr); }};
  return raw_ptr;
}

//  EntityView::add — two overloads, value-category-aware
//
//   e.add(arm)              → lvalue of copy-constructible T → COPIES into ECS
//   e.add(std::move(arm))   → explicit rvalue               → MOVES into ECS
//   e.add(T{...})           → temporary                     → MOVES into ECS
//   e.add(non_copyable_lv)  → compile error: must use std::move()
//
// "If you want to copy, show copy; if you want to move, show move (for
// non-copyable)."

// rvalue overload: move ownership into ECS
template <typename T>
  requires(!std::is_lvalue_reference_v<T>)
inline EntityView &EntityView::add(T &&component) {
  using RawT = std::remove_cvref_t<T>;
  world->add_component_dynamic(entity, get_type_info_ptr<RawT>(), &component);
  return *this;
}

// lvalue overload: copy into ECS, source object remains valid
template <typename T>
  requires std::copy_constructible<std::remove_cvref_t<T>>
inline EntityView &EntityView::add(const T &component) {
  using RawT = std::remove_cvref_t<T>;
  RawT copy = component; // explicit copy — no hidden allocation
  world->add_component_dynamic(entity, get_type_info_ptr<RawT>(), &copy);
  return *this;
}

template <typename T> inline EntityView &EntityView::remove() {
  world->remove_component_dynamic(entity, TypeTraits<T>::id);
  return *this;
}

template <typename T> inline T *EntityView::get() {
  return world->get_component<T>(entity);
}

inline void EntityView::despawn() { world->despawn(entity); }

inline Archetype<DefaultConfig> *EntityView::archetype() {
  auto res = world->index().lookup(entity);
  return res.is_ok()
             ? static_cast<Archetype<DefaultConfig> *>(res.unwrap()->archetype)
             : nullptr;
}

template <typename T> void World::add_resource(T &&res) {

  using RawT = std::remove_cvref_t<T>;
  uint64_t id = TypeTraits<RawT>::id;
  if (resources_.contains(id))
    return;

  // 1. 直接 new，让编译器处理对齐和大小
  RawT *raw_ptr = new RawT(std::forward<T>(res));

  // 2. 删除器里也直接 delete，它会自动调用析构并回收内存
  // static_cast 确保了编译器知道 RawT 的真实大小和对齐值 (alloc 4, dealloc 4)
  resources_[id] = {raw_ptr,
                    [](void *ptr) { delete static_cast<RawT *>(ptr); }};
}

template <typename T> T *World::get_resource() const {
  auto it = resources_.find(TypeTraits<std::remove_cvref_t<T>>::id);
  return (it != resources_.end()) ? static_cast<T *>(it->second.ptr) : nullptr;
}

template <typename T> T *World::get_component(Entity e) {
  auto res = index_.lookup(e);
  if (res.is_err())
    return nullptr;
  auto *arch = static_cast<Archetype<DefaultConfig> *>(res.unwrap()->archetype);
  auto col = arch->get_column_index(TypeTraits<T>::id);
  if (!col)
    return nullptr;
  auto loc = arch->table().locate(res.unwrap()->row);
  return static_cast<T *>(loc.chunk->component(*col, loc.index));
}

template <ValidQueryArg... Args>
template <typename Func>
void BoundQuery<Args...>::each(Func &&func) {
  world_->update_query(query_);
  query_.each(world_, std::forward<Func>(func));
}

template <ValidQueryArg... Args>
template <typename Func>
void BoundQuery<Args...>::iter(Func &&func) {
  world_->update_query(query_);
  query_.iter(world_, std::forward<Func>(func));
}

// ─── AutoQuery method definitions (World now complete) ───────────────────────

template <ValidQueryArg... Args>
template <typename Func>
void AutoQuery<Args...>::each(World *w, Func &&func) {
  w->update_query(*this);
  Query<Args...>::each(w, std::forward<Func>(func));
}

template <ValidQueryArg... Args>
template <typename Func>
void AutoQuery<Args...>::iter(World *w, Func &&func) {
  w->update_query(*this);
  Query<Args...>::iter(w, std::forward<Func>(func));
}

template <ValidQueryArg... Args> auto AutoQuery<Args...>::chunks(World *w) {
  w->update_query(*this);
  return Query<Args...>::chunks();
}

// ─── WorldView
// ──────────────────────────────────────────────────────────────── Thin
// parallel-safe wrapper around World*. Banned: spawn, despawn, scheduler
// internal CommandBuffers. Allowed: resources, entity reads/writes, query
// update.
struct ELYSIA_API WorldView {
public:
  explicit WorldView(World *w) noexcept : w_(w) {}

  ResourceAccessor resources() const noexcept { return {w_}; }
  EntityView entity(entity_t e) const { return {w_, e}; }

  template <typename Q> void update_query(Q &q) const { w_->update_query(q); }

  World *raw() const noexcept { return w_; }

private:
  World *w_;
};

// ─── AutoQuery WorldView overloads (WorldView now complete) ──────────────────

template <ValidQueryArg... Args>
template <typename Func>
void AutoQuery<Args...>::each(WorldView w, Func &&func) {
  w.update_query(*this);
  Query<Args...>::each(w.raw(), std::forward<Func>(func));
}

template <ValidQueryArg... Args>
template <typename Func>
void AutoQuery<Args...>::iter(WorldView w, Func &&func) {
  w.update_query(*this);
  Query<Args...>::iter(w.raw(), std::forward<Func>(func));
}

template <ValidQueryArg... Args> auto AutoQuery<Args...>::chunks(WorldView w) {
  w.update_query(*this);
  return Query<Args...>::chunks();
}

} // namespace elysia
