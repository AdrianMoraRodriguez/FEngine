# FEngine

A 3D game engine with a scene editor, built from scratch in C++20 and Vulkan.

> Work in progress. Currently setting up the project foundation.

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
| Build | CMake + vcpkg (manifest mode) |
| Windowing | GLFW |
| Math | GLM |
| GPU memory | Vulkan Memory Allocator |
| Editor UI | Dear ImGui (docking) |

## Building

Requirements:

- Visual Studio 2022 with the *Desktop development with C++* workload
- [Vulkan SDK](https://vulkan.lunarg.com/) (sets the `VULKAN_SDK` variable)
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set
cmake --preset windows
cmake --build --preset debug


## Status

Nothing runs yet. See the roadmap below.

## Roadmap

- [ ] Project scaffolding and window
- [ ] Vulkan instance, device, swapchain
- [ ] Frame loop with synchronization
- [ ] ImGui integration with docking
- [ ] Scene representation and serialization
- [ ] glTF mesh loading and PBR rendering
- [ ] Editor panels: hierarchy, inspector, viewport
- [ ] Shadows and lighting
- [ ] Skeletal animation
- [ ] Animation state machine

## Author

Adrián Mora Rodríguez — Master's in Video Game Development, Universidad de La Laguna.

## License

MIT — see [LICENSE](LICENSE).