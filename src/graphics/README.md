# Graphics Boundary

This folder will own Direct3D 11 and DXGI graphics primitives shared by capture and encoding:

- D3D11 device/context creation on the selected monitor adapter.
- DXGI device manager ownership.
- Texture pools.
- GPU scale and BGRA-to-NV12 conversion.
- Device-lost and resize handling.

Do not add CPU readback paths for the recording hot path.

Current implemented pieces:

- `DisplayManager`: enumerates Win32 monitors and maps a monitor to its DXGI adapter when possible.
- `D3D11DeviceContext`: creates a BGRA/video-capable D3D11 device for a selected monitor adapter.
- `GpuBgraToNv12`: validates BGRA source and even NV12 output dimensions, records the planned formats, smoke-tests app-owned GPU texture allocation, owns a reusable D3D11 VideoProcessor converter for caller-owned BGRA/NV12 textures, caches a bounded set of input views, reuses the fixed encoder output view, exposes view create/reuse/eviction counters, and exercises a BGRA-to-NV12 blit without adding CPU readback to the recording path.

Still deferred here:

- General-purpose scaling controls beyond the current VideoProcessor plan.
- Seamless resize and device-lost recovery.
