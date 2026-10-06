#include "scene/Components.h"

namespace fe {

void to_json(json& j, const glm::vec3& v) { j = json{v.x, v.y, v.z}; }
void from_json(const json& j, glm::vec3& v) {
    v = {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()};
}

void to_json(json& j, const glm::vec4& v) { j = json{v.x, v.y, v.z, v.w}; }
void from_json(const json& j, glm::vec4& v) {
    v = {j.at(0).get<float>(), j.at(1).get<float>(),
         j.at(2).get<float>(), j.at(3).get<float>()};
}

void to_json(json& j, const glm::quat& q) { j = json{q.w, q.x, q.y, q.z}; }
void from_json(const json& j, glm::quat& q) {
    q = {j.at(0).get<float>(), j.at(1).get<float>(),
         j.at(2).get<float>(), j.at(3).get<float>()};
}

} // namespace fe
