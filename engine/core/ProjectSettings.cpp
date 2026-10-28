#include "core/ProjectSettings.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace fe {

// The WITH_DEFAULT variant fills in any key the file does not have, so a file
// written before a setting existed still loads.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProjectSettings,
                                                distanceFieldResolution, distanceFieldDetail,
                                                meshFieldResolution,
                                                distanceFieldReflectionQuality,
                                                distanceFieldReflectionReach,
                                                distanceFieldShadowBias,
                                                distanceFieldReflectedShadows)

uint32_t ProjectSettings::meshFieldCells() const {
    static constexpr uint32_t kCells[kMeshFieldChoices] = {32, 64, 96, 128};
    return kCells[std::clamp(meshFieldResolution, 0, kMeshFieldChoices - 1)];
}

bool ProjectSettings::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;   // no file yet: the defaults stand

    try {
        nlohmann::json j;
        in >> j;
        *this = j.get<ProjectSettings>();
        return true;
    } catch (const nlohmann::json::exception& e) {
        FE_WARN("ProjectSettings: could not read '%s' (%s); using the current values",
                file.string().c_str(), e.what());
        return false;
    }
}

bool ProjectSettings::save(const std::filesystem::path& file) const {
    std::ofstream out(file);
    if (!out) {
        FE_WARN("ProjectSettings: could not write '%s'", file.string().c_str());
        return false;
    }
    out << nlohmann::json(*this).dump(4) << '\n';
    return static_cast<bool>(out);
}

} // namespace fe
