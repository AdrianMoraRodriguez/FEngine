#pragma once

/**
 * @file AssetDatabase.h
 * @brief Stable identity for asset files: an id that survives moving and
 *        renaming the file.
 *
 * ### The problem
 * A scene that says "this mesh is `models/tree.glb`" breaks the moment the
 * file becomes `props/tree.glb`. The Asset Browser lets the user move and
 * rename files freely, so references cannot be paths.
 *
 * ### The solution
 * Every model and every loadable image gets a 64-bit id, stored in a small
 * *sidecar* file next to it:
 *
 * @code
 *   assets/models/tree.glb
 *   assets/models/tree.glb.meta     <- {"version": 1, "id": "8f3a0c1d5b7e9a42"}
 * @endcode
 *
 * Scenes store the id. The database maps id <-> current path, and the
 * Asset Browser tells it whenever it moves, renames, copies or deletes
 * something, so the mapping always follows the file. The `.meta` files must be
 * committed to version control together with the assets: they ARE the
 * identity. They are hidden in the Asset Browser.
 *
 * Every id has its top bit set. MeshLibrary relies on it to tell an
 * imported model apart from the built-in primitives (handles 0, 1 and 2).
 *
 * ### Scenes saved before this existed
 * They stored a hash of the model's path, or the texture's path as text.
 * upgradeMeshHandle() and idFromLegacyPath() translate those on load, so old
 * scenes open normally and are written in the new format on the next save.
 *
 * Main thread only.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace fe {

class AssetDatabase {
public:
    /// An asset id; 0 means "none". Same type as fe::AssetHandle.
    using Id = std::uint64_t;

    /// Top bit of every id (see the file comment).
    static constexpr Id kIdBit = 0x8000000000000000ull;

    /// @return The one database of the editor.
    static AssetDatabase& instance();

    /// @return `true` for a `.meta` sidecar file. Meant for file listings.
    static bool isMetaFile(const std::filesystem::path& file);

    /// @return `true` if @p file is a kind of file that gets an id (a model
    ///         the importer supports, or an image the loader can decode).
    static bool isAsset(const std::filesystem::path& file);

    /**
     * @brief Registers every asset under the asset root (see assets::setRoot()).
     *
     * Reads the existing `.meta` files and creates the missing ones. Call
     * once at startup, after assets::setRoot().
     */
    void scan();

    /**
     * @brief Registers one file, or every file inside one folder.
     *
     * For when files arrive: a copy, a paste, a drop from the OS. A folder
     * copied together with its `.meta` files would hold the same ids twice;
     * the copies get new ones.
     */
    void scan(const std::filesystem::path& fileOrFolder);

    /**
     * @brief Returns the id of @p file, giving it one if it has none yet.
     * @return The id, or 0 if the file is not an asset or is outside the
     *         asset root.
     */
    Id idFor(const std::filesystem::path& file);

    /// @return The id of the asset at @p relativePath (asset-relative, '/'
    ///         separators), or 0 if there is none. Never creates anything.
    Id idOf(const std::string& relativePath) const;

    /// @return The current asset-relative path of @p id (UTF-8, '/'), or an
    ///         empty string if the id is unknown (the file was deleted).
    std::string pathOf(Id id) const;

    /// @return The full path of @p id, or an empty path if it is unknown.
    std::filesystem::path fullPathOf(Id id) const;

    /**
     * @brief Records that a file or folder was moved or renamed on disk.
     *
     * Call it right after the move succeeded. Moves the file's `.meta` along
     * and points every id inside at the new location.
     */
    void moved(const std::filesystem::path& from, const std::filesystem::path& to);

    /// Records that a file or folder was deleted from disk (call it after
    /// deleting). Removes the `.meta` of a file; ids inside are forgotten.
    void removed(const std::filesystem::path& path);

    /**
     * @brief Translates a mesh handle read from a scene saved by an older
     *        version (a hash of the model's path) to the model's id.
     * @return The id, or @p handle unchanged if it is already an id, is a
     *         built-in handle, or is unknown.
     */
    Id upgradeMeshHandle(Id handle) const;

    /**
     * @brief Resolves an asset-relative path stored by an older scene.
     * @return The id, or 0 if no asset is at that path.
     */
    Id idFromLegacyPath(const std::string& relativePath) const;

private:
    /// Registers @p file, reading or creating its `.meta`. @return its id or 0.
    Id registerFile(const std::filesystem::path& file);

    /// @return A new id nobody uses.
    Id makeId() const;

    /// Writes the sidecar of @p file. @return `false` (and warns once) on failure.
    bool writeMeta(const std::filesystem::path& file, Id id);

    std::unordered_map<std::string, Id> m_idByPath;
    std::unordered_map<Id, std::string> m_pathById;

    /// Old-style mesh handle (hash of the path at registration) -> id.
    std::unordered_map<Id, Id> m_legacyMesh;

    bool m_warnedWrite = false;
};

} // namespace fe
