#include "scene/UUID.h"

#include <random>

namespace fe {

UUID generateUUID() {
    static std::mt19937_64 engine{std::random_device{}()};
    static std::uniform_int_distribution<UUID> dist;

    UUID v = 0;
    // Zero is reserved as null; regenerate on the astronomically unlikely
    // collision.
    while (v == 0) {
        v = dist(engine);
    }
    return v;
}

} // namespace fe
