#pragma once

namespace fe {

struct Version {
    int major;
    int minor;
    int patch;
};

// Devuelve la versión del motor.
Version engineVersion();

// Cadena legible, p. ej. "FEngine 0.1.0"
const char* engineVersionString();

}
