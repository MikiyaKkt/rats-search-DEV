# Rats Search Documentation

## Quick Start (Qt 2.0)

1. **Download** the latest release from [GitHub Releases](https://github.com/DEgITx/rats-search/releases)
2. **Run** the application — it will automatically start indexing torrents from the DHT network
3. **Search** for torrents using the search bar
4. **Configure** ports and settings via File → Settings

## Documentation

### Configuration
* [Server Configuration](SERVER.md)
* [API Documentation](API.md)

## S2 download file selection

After adding a download, wait for metadata, then confirm its files in task settings. The file tree supports folder checkboxes, normal/high priorities and name/extension filtering. Select or skip all matching files, then click Confirm file selection. The task settings Save button saves bandwidth/queue policy independently. The file preview panel opens this same explicit dialog and does not change downloads while browsing.

Progress and ETA use verified bytes of selected files. Selected completion appears as partial seeding; full completion and remove-on-done retain full-download behavior. An all-skipped task stays idle. Use Recheck / retry files after deleting files or correcting a disk problem. See the [S2 validation checklist](DOWNLOAD_S2_VALIDATION.md) for recovery and boundary-data details.
## S3 advanced controls

Task settings now offer rarest-first, sequential or selected-file first/last-piece preference; bounded automatic/manual request windows; an aggregated piece map; and inherited or task-specific seeding conditions. These options do not change torrent block geometry or guarantee media preview. The map refreshes roughly every three seconds while the dialog is visible.

Global download settings offer an opt-in shared read cache (MiB), weekly bandwidth intervals with an IANA time zone, and default seeding ratio/time conditions. Schedule caps intersect with existing limits. Reaching a seeding goal keeps files and releases its slot; Restart seeding after goal starts a new accounting interval. Pause and queue time do not count as active seeding time.

Use the [S3 validation checklist](DOWNLOAD_S3_VALIDATION.md) and [implementation report](DOWNLOAD_S3_REPORT.md). For isolated portable runs use an ASCII database path and `--data-dir E:\RatsS3Test\data --no-remember-data-dir`. The bundled third-party database currently fails the Chinese data-directory startup smoke; Chinese payload names and download directories are tested separately and supported.