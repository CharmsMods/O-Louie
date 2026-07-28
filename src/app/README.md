# App Boundary

This folder owns process lifetime and command routing. `AppHost` is the narrow coordinator for startup, shutdown, current/legacy single-instance protection, runtime-data migration, logging, versioned settings load/apply, the hidden message window, tray/hotkey commands, settings-to-recorder configuration, and recorder-state notification marshalling.

Settings application validates first, transactionally replaces global hotkeys, synchronizes current-user startup registration, atomically persists the candidate, and rolls runtime changes back if a later step fails. Recorder-affecting changes rebuild the recorder immediately when inactive or after the active session finishes; clip settings and notifications update immediately.

Start/Stop, clip, bookmark, and save commands delegate to `record::VideoRecorderSession`; microphone-check commands delegate to `audio::MicMonitorSession`. `AppHost` enforces their mutual exclusion by asynchronously stopping live monitoring before dispatching a pending recording start. Expensive capture, playback, encode, recovery, and MP4 work remains off the message thread. Recording implementation, capture, encoding, audio, MP4 export, settings UI rendering, and packet storage do not belong here.
