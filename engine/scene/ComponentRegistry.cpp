#include "scene/ComponentRegistry.h"

namespace fe {

void registerBuiltinComponents() {
    auto& r = ComponentRegistry::instance();

    // Identity components are not removable — every entity must have them.
    r.add<NameComponent>("Name",       /*removable=*/ false);
    r.add<Transform>    ("Transform",  /*removable=*/ false);

    // Optional rendering components.
    r.add<MeshRenderer>    ("MeshRenderer");
    r.add<CameraComponent> ("Camera");
    r.add<LightComponent>  ("Light");
}

} // namespace fe
