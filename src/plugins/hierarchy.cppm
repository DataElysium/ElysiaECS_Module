module;
#include <vector>
#include <algorithm>
#include <iostream>

export module elysia.plugins.hierarchy;

import elysia;
import elysia.app; 

export namespace elysia {

struct ChildOf { Entity parent; };
struct Children { std::vector<Entity> ids; };

struct HierarchyPlugin {
    void build(App& app) {
        // 1. Link Child -> Parent
        app.observer<OnAdd, ChildOf>().run([&](Entity e) {
            auto* child_of = app.world().get_component<ChildOf>(e);
            if(!child_of) {
                std::cout << "[Hierarchy] ChildOf missing on entity " << e.id() << std::endl;
                return;
            }
            Entity parent = child_of->parent;
            // std::cout << "[Hierarchy] Linking child " << e.id() << " to parent " << parent.id() << std::endl;
            
            if(!app.world().index().is_alive(parent)) {
                std::cout << "[Hierarchy] Parent " << parent.id() << " is dead!" << std::endl;
                return;
            }

            auto* children = app.world().get_component<Children>(parent);
            if(!children) {
                // std::cout << "[Hierarchy] Adding Children component to parent " << parent.id() << std::endl;
                app.world().entity(parent).add(Children{});
                children = app.world().get_component<Children>(parent);
            }
            if(children) {
                children->ids.push_back(e);
                // std::cout << "[Hierarchy] Added. Parent now has " << children->ids.size() << " children." << std::endl;
            } else {
                std::cout << "[Hierarchy] FAILED to get Children component after adding!" << std::endl;
            }
        });

        // 2. Cascade Delete (Parent dies -> Children die)
        app.observer<OnRemove, Children>().run([&](Entity e) {
            auto* children = app.world().get_component<Children>(e);
            if(children) {
                // std::cout << "Cascade deleting children of " << e.id() << std::endl;
                for(Entity child : children->ids) {
                    if(app.world().index().is_alive(child)) {
                        app.world().despawn(child); 
                    }
                }
            }
        });
        
        // 3. Unlink (Child dies -> Remove from Parent)
        app.observer<OnRemove, ChildOf>().run([&](Entity e) {
             auto* child_of = app.world().get_component<ChildOf>(e);
             if(child_of && app.world().index().is_alive(child_of->parent)) {
                 auto* children = app.world().get_component<Children>(child_of->parent);
                 if(children) {
                     std::erase(children->ids, e);
                 }
             }
        });
    }
};

}