module;
#include <cstdio>
#include <functional>
#include <vector>
#include <unordered_map>
#include <cstdint>

export module elysia.observer;

import elysia.entity;
import elysia.meta;

export namespace elysia {

enum class ObserverEvent : uint8_t {
    OnAdd, OnRemove
};

// Hook keys
struct ObserverKey {
    ObserverEvent event;
    uint64_t component_id;

    bool operator==(const ObserverKey& other) const {
        return event == other.event && component_id == other.component_id;
    }
};

} // namespace elysia

// Hash for key
export namespace std {
    template<> struct hash<elysia::ObserverKey> {
        size_t operator()(const elysia::ObserverKey& k) const {
            return (static_cast<size_t>(k.component_id) << 1) | static_cast<size_t>(k.event);
        }
    };
}

export namespace elysia {

// Callback signature: void(Entity, void* component_data)
// Note: component_data might be null for OnRemove? No, we should try to provide it if possible (before removal).
// But for now, let's just pass Entity.
using ObserverCallback = std::function<void(Entity)>;

class ObserverRegistry {
public:
    void on_add(uint64_t comp_id, ObserverCallback cb) {
        observers_[{ObserverEvent::OnAdd, comp_id}].push_back(std::move(cb));
    }

    void on_remove(uint64_t comp_id, ObserverCallback cb) {
        observers_[{ObserverEvent::OnRemove, comp_id}].push_back(std::move(cb));
    }

    void notify(ObserverEvent event, uint64_t comp_id, Entity e) {
        auto it = observers_.find({event, comp_id});
        if (it == observers_.end()) return;
        
        for (const auto& cb : it->second) {
            cb(e);
        }
    }

    template<typename T>
    void on_add(ObserverCallback cb) {
        on_add(TypeTraits<std::remove_cvref_t<T>>::id, std::move(cb));
    }

    template<typename T>
    void on_remove(ObserverCallback cb) {
        on_remove(TypeTraits<std::remove_cvref_t<T>>::id, std::move(cb));
    }

    [[nodiscard]] bool has_observer(ObserverEvent event, uint64_t comp_id) const {
        return observers_.contains({event, comp_id});
    }

private:
    std::unordered_map<ObserverKey, std::vector<ObserverCallback>> observers_;
};

} // namespace elysia