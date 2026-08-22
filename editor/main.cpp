#include "core/Version.h"

#include <cstdio>

int main() {
  const fe::Version v = fe::engineVersion();

  std::printf("%s\n", fe::engineVersionString());
  std::printf("major=%d minor=%d patch=%d\n", v.major, v.minor, v.patch);
  std::printf("Prueba de build operativo.\n");

  return 0;
}
