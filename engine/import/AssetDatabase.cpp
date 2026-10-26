#include "import/AssetDatabase.h"

#include "core/AssetPaths.h"
#include "core/Log.h"
#include "core/PathUtil.h"
#include "import/ModelImporter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>

namespace fe {
namespace {

std::string lowerExtension(const std::filesystem::path& file) {
    std::string ext = toUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// The sidecar of @p file: the same name with ".meta" appended.
std::filesystem::path metaPath(const std::filesystem::path& file) {
    std::filesystem::path meta = file;
    meta += ".meta";
    return meta;
}

/// The id handle that scenes saved by an older version used for a model:
/// FNV-1a of the relative path, with the top bit set.
std::uint64_t legacyHandle(const std::string& relativePath) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : relativePath) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash | AssetDatabase::kIdBit;
}

std::string toHex(std::uint64_t id) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(id));
    return text;
}

/// Reads the id from a sidecar. @return 0 if it is missing, damaged, or not a valid id.
std::uint64_t readMeta(const std::filesystem::path& meta) {
    std::ifstream in(meta);
    if (!in) return 0;

    try {
        nlohmann::json j;
        in >> j;
        const std::string text = j.at("id").get<std::string>();
        const std::uint64_t id = std::strtoull(text.c_str(), nullptr, 16);
        return (id & AssetDatabase::kIdBit) ? id : 0;
    } catch (const nlohmann::json::exception&) {
        return 0;
    }
}

/// True if @p path is @p folder or lies inside it (both asset-relative, '/').
bool isInside(const std::string& path, const std::string& folder) {
    return path == folder ||
           (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 &&
            path[folder.size()] == '/');
}

} // namespace

AssetDatabase& AssetDatabase::instance() {
    static AssetDatabase database;
    return database;
}

bool AssetDatabase::isMetaFile(const std::filesystem::path& file) {
    return lowerExtension(file) == ".meta";
}

bool AssetDatabase::isAsset(const std::filesystem::path& file) {
    const std::string ext = lowerExtension(file);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" ||
        ext == ".hdr")
        return true;
    return isSupportedModel(file);
}

AssetDatabase::Id AssetDatabase::makeId() const {
    static std::mt19937_64 random{std::random_device{}()};
    for (;;) {
        const Id id = random() | kIdBit;
        if (m_pathById.count(id) == 0 && m_legacyMesh.count(id) == 0) return id;
    }
}

bool AssetDatabase::writeMeta(const std::filesystem::path& file, Id id) {
    nlohmann::json j;
    j["version"] = 1;
    j["id"]      = toHex(id);

    std::ofstream out(metaPath(file));
    if (out) {
        out << j.dump(2) << '\n';
        out.flush();
    }
    if (out) return true;

    if (!m_warnedWrite) {
        FE_WARN("AssetDatabase: cannot write '%s'; ids will not survive a restart",
                toUtf8(metaPath(file)).c_str());
        m_warnedWrite = true;
    }
    return false;
}

AssetDatabase::Id AssetDatabase::registerFile(const std::filesystem::path& file) {
    if (!isAsset(file)) return 0;

    const std::string relative = assets::toRelative(file);
    if (relative.empty()) return 0;

    if (const auto known = m_idByPath.find(relative); known != m_idByPath.end())
        return known->second;

    Id id = readMeta(metaPath(file));

    if (id != 0) {
        // The id may already belong to another file.
        if (const auto owner = m_pathById.find(id); owner != m_pathById.end()) {
            std::error_code ec;
            if (std::filesystem::exists(assets::resolve(owner->second), ec)) {
                // Two files claim it: a folder was copied with its sidecars.
                // The one found first keeps the id.
                FE_INFO("AssetDatabase: '%s' repeats the id of '%s'; giving it a new one",
                        relative.c_str(), owner->second.c_str());
                id = 0;
            } else {
                // The other file is gone: this one was moved outside the
                // editor, together with its sidecar. The id follows it.
                m_idByPath.erase(owner->second);
                m_pathById.erase(owner);
            }
        }
    }

    if (id == 0) {
        id = makeId();
        writeMeta(file, id);
    }

    m_idByPath[relative] = id;
    m_pathById[id]       = relative;
    if (isSupportedModel(file)) m_legacyMesh[legacyHandle(relative)] = id;
    return id;
}

void AssetDatabase::scan() {
    if (!assets::root().empty()) scan(assets::root());

    FE_INFO("AssetDatabase: %zu asset(s) in '%s'",
            m_idByPath.size(), toUtf8(assets::root()).c_str());
}

void AssetDatabase::scan(const std::filesystem::path& fileOrFolder) {
    std::error_code ec;
    if (!std::filesystem::is_directory(fileOrFolder, ec)) {
        registerFile(fileOrFolder);
        return;
    }

    // Advanced with increment(ec): the range-for operator++ throws when one
    // entry cannot be read, and one odd file must not stop the editor.
    std::filesystem::recursive_directory_iterator it(
        fileOrFolder, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;

    for (; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_regular_file(entryEc)) registerFile(it->path());
    }
    if (ec) FE_WARN("AssetDatabase: stopped scanning early: %s", ec.message().c_str());
}

AssetDatabase::Id AssetDatabase::idFor(const std::filesystem::path& file) {
    return registerFile(file);
}

AssetDatabase::Id AssetDatabase::idOf(const std::string& relativePath) const {
    const auto found = m_idByPath.find(relativePath);
    return found != m_idByPath.end() ? found->second : 0;
}

std::string AssetDatabase::pathOf(Id id) const {
    const auto found = m_pathById.find(id);
    return found != m_pathById.end() ? found->second : std::string();
}

std::filesystem::path AssetDatabase::fullPathOf(Id id) const {
    const std::string relative = pathOf(id);
    return relative.empty() ? std::filesystem::path() : assets::resolve(relative);
}

void AssetDatabase::moved(const std::filesystem::path& from, const std::filesystem::path& to) {
    const std::string oldRel = assets::toRelative(from);
    const std::string newRel = assets::toRelative(to);

    // A file takes its sidecar with it. (A folder's sidecars are inside it.)
    std::error_code ec;
    const std::filesystem::path oldMeta = metaPath(from);
    if (std::filesystem::is_regular_file(oldMeta, ec) &&
        !std::filesystem::is_directory(to, ec)) {
        const std::filesystem::path newMeta = metaPath(to);
        std::filesystem::remove(newMeta, ec);   // a stale one from an older file
        std::filesystem::rename(oldMeta, newMeta, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(oldMeta, newMeta, ec);
            if (!ec) std::filesystem::remove(oldMeta, ec);
        }
    }

    if (oldRel.empty()) {
        // Came from outside the asset root: nothing to follow, just register it.
        scan(to);
        return;
    }

    // Re-point every id at or below the old location.
    std::vector<std::pair<std::string, Id>> affected;
    for (const auto& [path, id] : m_idByPath)
        if (isInside(path, oldRel)) affected.emplace_back(path, id);

    for (const auto& [path, id] : affected) {
        m_idByPath.erase(path);
        if (newRel.empty()) {   // moved out of the asset root
            m_pathById.erase(id);
            continue;
        }
        const std::string target = newRel + path.substr(oldRel.size());
        m_idByPath[target] = id;
        m_pathById[id]     = target;
    }
}

void AssetDatabase::removed(const std::filesystem::path& path) {
    const std::string relative = assets::toRelative(path);
    if (relative.empty()) return;

    std::error_code ec;
    std::filesystem::remove(metaPath(path), ec);   // no-op for folders

    std::vector<std::string> gone;
    for (const auto& entry : m_idByPath)
        if (isInside(entry.first, relative)) gone.push_back(entry.first);

    for (const std::string& p : gone) {
        m_pathById.erase(m_idByPath[p]);
        m_idByPath.erase(p);
    }
}

AssetDatabase::Id AssetDatabase::upgradeMeshHandle(Id handle) const {
    if ((handle & kIdBit) == 0) return handle;          // none or built-in
    if (m_pathById.count(handle)) return handle;        // already an id

    const auto legacy = m_legacyMesh.find(handle);
    return legacy != m_legacyMesh.end() ? legacy->second : handle;
}

AssetDatabase::Id AssetDatabase::idFromLegacyPath(const std::string& relativePath) const {
    return idOf(relativePath);
}

} // namespace fe
