# Rats Search Documentation

## Quick Start (Qt 2.0)

1. **Download** the latest release from [GitHub Releases](https://github.com/DEgITx/rats-search/releases)
2. **Run** the application — it will automatically start indexing torrents from the DHT network
3. **Search** for torrents using the search bar
4. **Configure** ports and settings via File → Settings

## Documentation

### Searching torrents and files

Enter `电影` to find a continuous Chinese term. Separate conditions with spaces
to require each one in any order, as in `电影 1080p`. Enclose a phrase in straight
or curly double quotes to require adjacent words in order, as in `"Ubuntu Linux"`.
An asterisk outside quotes supports title prefixes, as in `ubun*`. File search
matches all conditions on one file path. Its results highlight that path.

Enable **Fuzzy** to supplement ordinary results with one-character corrections
such as `ubntu` → `ubuntu`. It is off when the app starts. Fuzzy results are
marked and appear after ordinary results. Quotes, wildcards and Info Hash lookups
keep their original matching rules.

Desktop search also asks connected Rats peers. Peers that advertise advanced
search can answer short continuous terms, phrases and fuzzy requests. A new
search, filter or sort gets a new request ID, so late replies from upgraded peers
are discarded. Older peers still answer compatible ordinary searches. Their
legacy replies may arrive without an ID and can appear after an ordinary search
has been changed. [Search API](API.md#search)

The first start after upgrading an older index may show search-index rebuilding
progress. If the app is interrupted, the next start resumes from the last saved
batch.

### Crawler concurrency

In Settings → Indexer → Spider Performance, **Metadata concurrency limit** controls
how many crawler metadata requests may run together (1–64, default 10). The current
activity label shows the saved limit separately from active requests. Pending edits
take effect after Save without restarting; reducing the limit allows existing
requests to finish before more are started. Hover over the main window's Spider
indicator for the same activity information.

**Pending task limit** (1–1,000) covers hash lookups, result reservations, database
writes and retries. **Database batch size** (1–100) controls the maximum task count
per database batch; sparse batches still submit after 100 ms. Keep the defaults
unless measurements show a benefit. Larger concurrency does not guarantee faster
indexing: each metadata request first reserves an estimated 4 MiB from the shared
64 MiB budget, and backpressure or database/notification throughput may limit it.
These are estimated payload limits rather than a process memory/RSS cap.

The same controls are available in the [configuration API](API.md). See the
[P4 implementation report](CRAWLER_P4_REPORT.md) for measurements and limitations.

### Crawler overload, retries and shutdown

The crawler is a best-effort discovery service. Its 5,000 discovery slots include
queued announcements and active/retrying candidates; a full entrance drops new
announcements and counts the rejection. Duplicate announcements can add up to
three peer addresses without creating another active candidate. The metadata
waiting queue is also bounded (1,024 requests / an estimated 1 MiB).

At 80% of either task or byte capacity, new metadata requests pause. They resume
only after both capacities fall to 50% or lower, without needing a new
announcement. Already reserved results continue into the database. A network
result exceeding its 4 MiB estimate limit ends without retry; database rows
exceeding the SQL statement limit are also terminal. These estimates do not cap
process RSS or the database daemon's memory.

Timeouts retry through the bounded peer list; temporary database failures retry
using the metadata already fetched. Each layer allows at most three retries,
with exponential backoff, jitter and a deadline for starting further retries.
The retry queues share normal capacity. Invalid data, filter rejection and row-ID
collisions do not retry. A long outage can exhaust retries, so recovery does not
guarantee every previously announced hash is inserted.

Stopping cancels pending/retrying work and undelivered notifications. A SQL write
already executing may commit before its notification is cancelled; committed
rows remain, and database statistics are restored from storage on restart.
Unstarted tasks and retry state are held in memory and are not a durable queue.
Network results from an older run cannot enter the new run. Their reserved
capacity stays charged until the callbacks settle or transport shutdown joins
them. The application joins workers and network callbacks before closing the
database; it never force-kills a database worker to meet a UI timeout.

See [P5 validation](CRAWLER_P5_REPORT.md) for deterministic fault tests, lifecycle
and soak evidence, and the limits of the network measurements.

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
