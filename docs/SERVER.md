# Server Configuration

This document covers server configuration for both Qt 2.0 and the legacy Node.js version.

---

## Rats Search 2.0 (Qt Version)

### Built-in API Server

Rats Search 2.0 includes a built-in REST/WebSocket API server that can be enabled in the settings.

**Enable via UI:** File → Settings → Network → "Enable REST API server"

**Or via configuration file** (`rats.json`):

```json
{
    "restApiEnabled": true,
    "httpPort": 8095
}
```

### Configuration File Location

The configuration file `rats.json` is created in the data directory:

| Platform | Location |
|----------|----------|
| Windows | `%APPDATA%/RatsSearch/rats.json` or application directory |
| Linux | `~/.config/RatsSearch/rats.json` or `~/.local/share/RatsSearch/` |
| macOS | `~/Library/Application Support/RatsSearch/rats.json` |

### Qt 2.0 Configuration Options

```json
{
    "p2pPort": 4445,
    "dhtPort": 4446,
    "httpPort": 8095,
    "restApiEnabled": true,
    "indexerEnabled": true,
    "trackersEnabled": true,
    "darkMode": true,
    "trayOnMinimize": true,
    "trayOnClose": false,
    "confirmExit": true,
    "startMinimized": false
}
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `p2pPort` | int | 4445 | P2P communication port |
| `dhtPort` | int | 4446 | DHT operations port |
| `httpPort` | int | 8095 | REST API server port |
| `restApiEnabled` | bool | false | Enable REST API server |
| `indexerEnabled` | bool | true | Enable DHT torrent indexer |
| `trackersEnabled` | bool | true | Enable tracker peer checking |
| `darkMode` | bool | true | Use dark theme |
| `uiFontFamily` | string | "" | Interface font family, tried before the theme's own font stack (empty = theme default) |
| `uiFontScale` | int | 100 | Interface text size in percent of the default (clamped to 50–200) |
| `trayOnMinimize` | bool | true | Hide to tray on minimize |
| `trayOnClose` | bool | false | Hide to tray on close |
| `confirmExit` | bool | true | Ask for confirmation before quitting the GUI |
| `startMinimized` | bool | false | Start minimized to tray |

---

## Legacy Version (Node.js)

> The legacy Electron/Node.js version is located in the `legacy/` folder.

### Data Directory Configuration

File `package.json` contains path to data directory. You can move this folder to any location (by default, data directory for web server is the same as root folder with package.json):

```json
{
    "serverDataDirectory": "./"
}
```

This directory always contains logs and other configuration entities.

### Legacy Configuration

After server start, `rats.json` will be created in the root folder:

```json
{
    "dbPath": "/path/to/db",
    "httpPort": 8095
} 
```

| Option | Description |
|--------|-------------|
| `httpPort` | Port which HTTP server listens on |
| `dbPath` | Path to database folder with torrent collection |

## S2 headless download confirmation

Headless clients must fetch task metadata and submit a complete `download.files.set` selection with the current `selectionRevision` as `expectedRevision`. Content remains waiting until confirmed. Poll `download.operation.get` until the operation is terminal; do not treat accepted as committed. See [API](API.md#s2-selective-downloads). Preserve the `.parts` directory together with payload and resume/session data when moving or backing up partial downloads. Sessions without selection confirmation require manual confirmation.
## S3 scheduling and seeding

See [advanced API contracts](API.md#s3-advanced-download-policies) for version-2 policy/session migration, weekly schedules, bounded request windows, cache budgets, piece aggregation and seeding goal ledgers. Use expected revisions when updating schedules or seeding conditions; accepted file operations remain asynchronous. Schedule pause preserves manual pause intent and does not imply a process-wide network blackout. Seeding goals never remove content. Keep a backup of configuration, sessions, resume and `.parts` together.

For isolated verification, `--no-remember-data-dir` prevents an explicit `--data-dir` from replacing the user's remembered location. When managing bundled Manticore manually for a Unicode Windows profile, run its commands from the profile directory and pass `--config sphinx.conf`.
