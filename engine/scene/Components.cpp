#include "scene/Components.h"

namespace glm {

void to_json(nlohmann::json& j, const vec3& v) { j = {v.x, v.y, v.z}; }
void from_json(const nlohmann::json& j, vec3& v) {
    v = {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()};
}

void to_json(nlohmann::json& j, const vec4& v) { j = {v.x, v.y, v.z, v.w}; }
void from_json(const nlohmann::json& j, vec4& v) {
    v = {j.at(0).get<float>(), j.at(1).get<float>(),
         j.at(2).get<float>(), j.at(3).get<float>()};
}

void to_json(nlohmann::json& j, const quat& q) { j = {q.w, q.x, q.y, q.z}; }
void from_json(const nlohmann::json& j, quat& q) {
    q = {j.at(0).get<float>(), j.at(1).get<float>(),
         j.at(2).get<float>(), j.at(3).get<float>()};
}

} // namespace glm
