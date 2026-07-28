# Logging And Runtime Paths Boundary

This folder owns runtime data placement and file logging.

Current runtime folders are rooted under `%LOCALAPPDATA%\O'Louie`:

- `settings`
- `logs`
- `sessions`
- `exports`
- `cache`

`RuntimePathMigration` resolves this root before logger or settings startup. It atomically moves the complete legacy root when the new root is absent, preserves both roots on conflict, and falls back to the legacy root for the current run if the rename is blocked. Saved paths under a successfully migrated root can be rebased without touching unrelated custom destinations.

Future work should keep durable settings, recoverable session data, output files, logs, and disposable cache data separate.
