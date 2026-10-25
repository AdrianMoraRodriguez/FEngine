# FEngine

A 3D game engine with a scene editor, built from scratch in C++20 and Vulkan.

> Work in progress. The editor runs and can build, save and load scenes; PBR materials and animation are next.

## Features

- **Editor:** dockable panels for the scene hierarchy, inspector, viewport and statistics, with transform gizmos, click-to-select, a selection outline and a free-fly camera.
- **Asset browser:** modelled on a file explorer: navigation history and breadcrumbs, search, icon and list views, sortable columns, multi-selection, copy/cut/paste, drag and drop (from the OS or between folders), thumbnails of models and images.
- **Viewers:** images open in their own dockable tab (zoom, pan, fit).
- **Scenes:** entity hierarchy on top of an ECS, saved and loaded as JSON.
- **Models:** import of glTF 2.0 and the formats supported by Assimp, with UVs.
- **Materials:** base colour and albedo texture per object, assigned by dropping an image on the Inspector.
- **Lighting:** directional, point and spot lights with shadows (cascaded shadow maps for the sun).

## Goals

- Editor-first workflow: scene hierarchy, inspector, gizmos, asset browser
- Multi-scene projects with JSON serialization
- glTF 2.0 asset pipeline with PBR materials
- Skeletal animation driven by a visual state machine
- Hybrid rendering: rasterized meshes alongside raymarched SDF volumes

## Tech stack

| Area | Choice |
|---|---|
| Language | C++20 |
| Graphics API | Vulkan 1.3 (dynamic rendering, synchronization2) |
| Build | CMake + vcpkg (manifest mode), Ninja Multi-Config |
| Windowing | GLFW |
| Math | GLM |
| GPU memory | Vulkan Memory Allocator |
| Editor UI | Dear ImGui (docking) + ImGuizmo |
| Scene | EnTT, nlohmann/json |
| Assets | Assimp, cgltf, stb_image |

## Building

Requirements:

- Visual Studio 2022 with the *Desktop development with C++* workload
- [Vulkan SDK](https://vulkan.lunarg.com/) (sets the `VULKAN_SDK` variable)
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set

```
cmake --preset windows
cmake --build --preset debug
```

## Roadmap

- [x] Project scaffolding and window
- [x] Vulkan instance, device, swapchain
- [x] Frame loop with synchronization
- [x] ImGui integration with docking
- [x] Scene representation and serialization
- [x] glTF mesh loading
- [x] Editor panels: hierarchy, inspector, viewport, asset browser
- [x] Shadows and lighting
- [x] Textured materials (albedo)
- [ ] PBR materials from glTF
- [ ] Stable asset IDs (moving or renaming an asset keeps scene references)
- [ ] 3D model viewer tab
- [ ] Skeletal animation
- [ ] Animation state machine

## Author

Adrián Mora Rodríguez — Master's in Video Game Development, Universidad de La Laguna.

## License

MIT — see [LICENSE](LICENSE).
