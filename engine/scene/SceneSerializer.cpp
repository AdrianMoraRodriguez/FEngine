#include "scene/SceneSerializer.h"

#include "scene/ComponentRegistry.h"
#include "core/Log.h"

#include <fstream>
#include <iterator>

namespace fe {
namespace {

// Writes one entity and then recurses into its children.
// Depth-first order guarantees parents appear before children in the array,
// which lets loadFromFile resolve parent UUIDs without a two-pass approach.
void saveEntity(const Scene& scene, entt::entity e, json& array) {
    const auto& reg = scene.registry();

    json je;
    je["uuid"] = reg.get<IDComponent>(e).uuid;

    // Parent UUID: zero means "no parent" (root entity).
    entt::entity parent = reg.get<Relationship>(e).parent;
    je["parent"] = (parent != entt::null)
        ? reg.get<IDComponent>(parent).uuid
        : UUID(0);

    // Serialise every registered component present on this entity.
    json components = json::object();
    for (const auto& meta : ComponentRegistry::instance().all()) {
        if (!meta.has(reg, e)) continue;
        json jc;
        meta.save(reg, e, jc);
        components[meta.name] = std::move(jc);
    }
    je["components"] = std::move(components);
    array.push_back(std::move(je));

    // Recurse into children in sibling order.
    for (entt::entity c = reg.get<Relationship>(e).firstChild;
         c != entt::null;
         c = reg.get<Relationship>(c).nextSibling)
        saveEntity(scene, c, array);
}

} // namespace

std::string SceneSerializer::serialize(const Scene& scene) {
    json out;
    out["version"]  = 1;
    out["name"]     = scene.name();
    out["settings"] = scene.settings();
    out["entities"] = json::array();

    const auto& reg = scene.registry();
    for (entt::entity e = scene.firstRoot(); e != entt::null;
         e = reg.get<Relationship>(e).nextSibling)
        saveEntity(scene, e, out["entities"]);

    return out.dump(2);
}

bool SceneSerializer::saveToFile(const Scene& scene,
                                 const std::filesystem::path& path) {
    std::ofstream file(path);
    if (!file) {
        FE_ERROR("SceneSerializer: cannot open '%s' for writing",
                 path.string().c_str());
        return false;
    }

    file << serialize(scene);
    file.flush();
    if (!file) {
        FE_ERROR("SceneSerializer: failed while writing '%s'",
                 path.string().c_str());
        return false;
    }

    FE_INFO("Scene '%s' saved to '%s'",
            scene.name().c_str(), path.string().c_str());
    return true;
}

bool SceneSerializer::loadFromFile(Scene& scene,
                                   const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        FE_ERROR("SceneSerializer: cannot open '%s' for reading",
                 path.string().c_str());
        return false;
    }

    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    if (!loadFromString(scene, text, path.string())) return false;

    FE_INFO("Scene '%s' loaded from '%s'",
            scene.name().c_str(), path.string().c_str());
    return true;
}

bool SceneSerializer::loadFromString(Scene& scene, const std::string& text,
                                     const std::string& label) {
    // Any malformed or incomplete content surfaces as a json::exception
    // (missing keys, wrong types), so the whole load is guarded: a bad file
    // must be rejected, never crash the editor.
    try {
        const json in = json::parse(text);

        if (in.value("version", 0) != 1) {
            FE_ERROR("SceneSerializer: unsupported scene version in '%s'",
                     label.c_str());
            return false;
        }

        scene.setName(in.value("name", std::string{"Untitled"}));

        // Scenes saved before the settings existed have no such key.
        scene.settings() = in.contains("settings") ? in.at("settings").get<SceneSettings>()
                                                   : SceneSettings{};

        // Entities are stored in depth-first order (parents before
        // children), so resolving the parent UUID always succeeds.
        for (const auto& je : in.at("entities")) {
            UUID uuid     = je.at("uuid").get<UUID>();
            UUID parentId = je.value("parent", UUID(0));

            entt::entity parent = (parentId != 0)
                ? scene.findByUUID(parentId)
                : entt::null;

            std::string name = "Entity";
            const auto& comps = je.at("components");
            if (comps.contains("Name"))
                name = comps.at("Name").at("name").get<std::string>();

            entt::entity e = scene.createWithUUID(uuid, name, parent);

            for (auto it = comps.begin(); it != comps.end(); ++it) {
                const ComponentMeta* meta =
                    ComponentRegistry::instance().find(it.key());
                if (meta)
                    meta->load(scene.registry(), e, it.value());
                else
                    FE_WARN("SceneSerializer: unknown component '%s' skipped",
                            it.key().c_str());
            }

            scene.registry().get<WorldTransform>(e).dirty = true;
        }
    } catch (const json::exception& ex) {
        FE_ERROR("SceneSerializer: invalid scene file '%s': %s",
                 label.c_str(), ex.what());
        return false;
    }

    return true;
}

} // namespace fe
