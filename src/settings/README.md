# Settings And Presets Boundary

This folder owns settings data shapes, default values, validation, hotkey parsing, migrations, and persistence.

`SettingsStore` reads and atomically replaces versioned `settings.json` through structured Windows JSON APIs. Schema version 3 adds the video recording performance mode; version-1 and version-2 files migrate in memory to Balanced, while version-1 also migrates microphone-check playback to Windows Default. Older files are rewritten only after an explicit save. Missing data uses defaults; malformed, invalid, oversized, inaccessible, and unsupported-version files return distinct diagnostics and safe defaults. Unsupported data is not overwritten automatically.

`HotkeyBinding` parses supported global combinations into canonical labels and Win32 modifier/key values. Settings validation requires unique supported bindings, valid recorder/video/audio/clip values, and the current hardware-only encoder direction.
