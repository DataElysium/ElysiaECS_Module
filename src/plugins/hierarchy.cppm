export module elysia.plugins.hierarchy;
import elysia.app;
export import elysia.hierarchy;

export namespace elysia {
struct HierarchyPlugin {
    void build(App& app) { install_hierarchy(app.world()); }
};
}
