#pragma once

/**
 * @file ComponentRegistry.h
 * @brief Self-registering metadata for all built-in ECS components.
 *
 * Each component is registered once via ComponentRegistry::add<T>(). From
 * that registration the editor gets, for free:
 *   - Inspector UI (via the inspect() overload in Inspectors.cpp)
 *   - JSON serialisation and deserialisation
 *   - "Add Component" menu entries
 *   - SceneSerializer support
 *
 * Adding a new component requires:
 *   1. Declaring the struct in Components.h.
 *   2. Adding NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE in Components.h.
 *   3. Adding an inspect() overload in Inspectors.cpp.
 *   4. Calling ComponentRegistry::instance().add<T>() in
 *      registerBuiltinComponents().
 */

#include "scene/Components.h"

#include <functional>
#include <string>
#include <vector>

namespace fe {

/**
 * @brief Metadata and runtime operations for a single component type.
 *
 * All function pointers are populated by ComponentRegistry::add<T>()
 * through lambda captures — no virtual dispatch, no RTTI.
 */
struct ComponentMeta {
    std::string name;       ///< Identifier used in JSON and the UI.
    bool removable = true;  ///< False for IDComponent, NameComponent, etc.

    /// Adds the component to @p e with default values (if not already present).
    std::function<void(entt::registry&, entt::entity)> add;

    /// Removes the component from @p e.
    std::function<void(entt::registry&, entt::entity)> remove;

    /// Returns true if @p e has this component.
    std::function<bool(const entt::registry&, entt::entity)> has;

    /// Serialises the component into @p j.
    std::function<void(const entt::registry&, entt::entity, json&)> save;

    /// Deserialises the component from @p j and emplaces it on @p e.
    std::function<void(entt::registry&, entt::entity, const json&)> load;

    /// Draws the ImGui inspector widgets for the component.
    std::function<void(entt::registry&, entt::entity)> inspect;
};

/**
 * @brief Singleton registry of all component types known to the engine.
 *
 * Populated at startup by registerBuiltinComponents(). New component types
 * are added by calling add<T>() during that function.
 */
class ComponentRegistry {
public:
    /// @return The single global registry instance.
    static ComponentRegistry& instance() {
        static ComponentRegistry reg;
        return reg;
    }

    /**
     * @brief Registers a component type and generates its runtime operations.
     *
     * @tparam T        The component struct to register.
     * @param  name     The string identifier used in JSON and the UI.
     * @param  removable Whether the inspector should show a Remove button.
     *
     * @note T must have:
     *   - nlohmann::json serialisation (to_json / from_json)
     *   - An fe::inspect(T&) overload
     */
    template <typename T>
    void add(const std::string& name, bool removable = true) {
        ComponentMeta meta;
        meta.name      = name;
        meta.removable = removable;

        meta.add = [](entt::registry& reg, entt::entity e) {
            if (!reg.all_of<T>(e)) reg.emplace<T>(e);
        };

        meta.remove = [](entt::registry& reg, entt::entity e) {
            reg.remove<T>(e);
        };

        meta.has = [](const entt::registry& reg, entt::entity e) {
            return reg.all_of<T>(e);
        };

        meta.save = [](const entt::registry& reg, entt::entity e, json& j) {
            j = reg.get<T>(e);
        };

        meta.load = [](entt::registry& reg, entt::entity e, const json& j) {
            reg.emplace_or_replace<T>(e, j.get<T>());
        };

        meta.inspect = [](entt::registry& reg, entt::entity e) {
            fe::inspect(reg.get<T>(e));
        };

        m_metas.push_back(std::move(meta));
    }

    /// @return All registered component metadata, in registration order.
    const std::vector<ComponentMeta>& all() const { return m_metas; }

    /**
     * @brief Finds metadata by component name.
     * @return Pointer to the metadata, or nullptr if not found.
     */
    const ComponentMeta* find(const std::string& name) const {
        for (const auto& m : m_metas)
            if (m.name == name) return &m;
        return nullptr;
    }

private:
    std::vector<ComponentMeta> m_metas;
};

/**
 * @brief Registers all built-in engine components.
 *
 * Must be called once before any scene is created, serialised or displayed
 * in the inspector. Typically called at the start of main().
 */
void registerBuiltinComponents();

} // namespace fe
