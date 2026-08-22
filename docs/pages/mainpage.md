# FEngine {#mainpage}

A 3D game engine with a scene editor, built from scratch in C++20 and Vulkan.

## Overview

FEngine is split into two layers:

- **Engine** — a static library containing platform abstraction, rendering,
  scene management and asset handling. Has no dependency on the editor.
- **Editor** — an executable providing the authoring interface: scene
  hierarchy, inspector, viewport and asset browser.

A third target, **Runtime**, will ship games without the editor. Because of
this, nothing in the engine layer may include editor headers.

## Where to start

- fe::Window — platform window and Vulkan surface provider
- fe::Version — engine version information

## Conventions

- Interfaces are documented in headers, not in source files.
- Types live in namespace `fe`.
- Macros are prefixed `FE_`.
