#include "core/Version.h"

namespace fe {

  Version engineVersion() {
    return Version{0, 1, 0};
  }

  const char* engineVersionString() {
    return "FEngine 0.1.0";
  }

}
