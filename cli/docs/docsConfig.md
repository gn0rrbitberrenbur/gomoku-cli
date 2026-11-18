# CLI configuration
## Purpose
Manage registered external executables and validate settings before launching a game.

## Files
- [settings.hpp](../moku-cli/settings/settings.hpp), [settings.cpp](../moku-cli/settings/settings.cpp)
- Per-game setup is owned by [Application](../moku-cli/ui/application.hpp).

## Interface
`EngineEntry` stores a display name and executable path. `EngineStore` loads the registry on
construction and exposes `add`, `edit` and `remove`. Each mutation validates, saves a new list,
and only commits the in-memory change if saving succeeds.

Names must be nonempty, unique, at most 80 bytes, and free of control characters. New and edited
paths must refer to existing files; POSIX additionally checks execute permission. Relative paths
are resolved when saving, not when playing. Missing executable files in an existing registry do
not prevent editing/removing those entries; game setup revalidates the selected engines.

## Storage
Defaults are `%APPDATA%\moku-cli\engines.cfg` on Windows and
`$XDG_CONFIG_HOME/moku-cli/engines.cfg` or `$HOME/.config/moku-cli/engines.cfg` on Linux.
`--config <path>` overrides the location. If no default directory is available, the application
reports an error instead of silently choosing a temporary location.

The format is UTF-8 with a version header and three fields per line: the quoted name, the quoted
path, and the lowercase SHA-256 hash of the executable:

```text
MOKU_ENGINES 2
"Example engine" "C:\\engines\\example.exe" a3f2...
```

Legacy `MOKU_ENGINES 1` registries are loaded as a compatibility format and are migrated to
version 2 by calculating and storing the hash for each existing executable. On registration and
editing, the hash is calculated immediately. Before an engine is started anywhere in the CLI,
`EngineStore::validate` recalculates its SHA-256 hash and rejects the entry if the binary changed
or the stored hash is malformed.

Writes use `engines.cfg.tmp` next to the destination and an atomic replacement through the shared
`writeFileAtomically` helper ([file_io.hpp](../moku-cli/file_handling/file_io.hpp)), also used for test reports.
Parent directories
are created as needed. Only one running application should edit a particular registry.
Windows replacement retries transient sharing/access locks for up to one second; persistent
failures are reported and leave the in-memory entries unchanged.
Malformed or unreadable existing files are reported without overwriting them. Back up or repair
the file, or use `--config` with another path to recover.

## Per-game parameters
Board size is 5-19, engine time is 1-600000 milliseconds, and the rule is freestyle or exact five.
Defaults are 15, 5000 milliseconds and freestyle. Human players may choose Black or White.
Engine selection is mandatory for each slot on every new game; both slots may select the same
registered executable, in which case two independent workers/processes are created.

Game settings are session-local and are not written to the engine registry.

## Validation
The core test executable covers quoted persistence, duplicate names, invalid paths, edits, removal,
malformed files and failures that must not mutate memory.