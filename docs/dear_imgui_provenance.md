# Dear ImGui Development Dependency Provenance

- Project: Dear ImGui
- Version/tag: `v1.92.8`
- Upstream: `ocornut/imgui`
- Official release page: `https://github.com/ocornut/imgui/releases/tag/v1.92.8`
- Source archive: `https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.zip`
- Source archive SHA-256: `27765c56ab27ce47472d0bea43cf1e3301c726362ce585e99a059e3b37616870`
- License: MIT (`LICENSE.txt` in the source archive)
- Local development root: `_deps\imgui`
- Acquisition tool: `tools\PrepareDearImGui.ps1`

O'Louie compiles the core Dear ImGui sources plus the official Win32 and DirectX 11 backends directly into the application. The ignored `_deps\imgui` source tree is a pinned development dependency; release packaging and third-party notice placement remain part of the later packaging/compliance pass.
