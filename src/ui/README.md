# UI Boundary

This folder owns the Dear ImGui settings UI and UI-specific rendering host.

`ImGuiDx11Host` owns the top-level Win32 window, D3D11 swap chain, official Dear ImGui Win32/DX11 backends, capture exclusion, and a 33 ms visible-only render timer. It creates no hidden frames, does no rendering from `WM_PAINT`, and keeps lifecycle counters for focused verification.

`SettingsUi` owns the General, Video, Audio, Clips/Bookmarks, and Hotkeys editor. It edits a draft, validates/canonicalizes text and hotkeys, applies through the app-owned transactional callback, supports cancel/reset, and reuses the dormant host across opens. The Audio tab enumerates render endpoints and presents app-owned microphone-monitor commands and snapshots; it never performs WASAPI work on the UI thread.

The notification overlay is a separate Win32 shell concern unless a later implementation needs shared UI rendering code.
