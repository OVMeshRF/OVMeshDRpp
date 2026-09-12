# ADR-0010: RF measurements and metadata-only classification

Status: accepted scope, 2026-09-11. Supersedes semantic-message interpretation and content-retention provisions in earlier decisions.

## Decision

Preserve spectrum measurements, receiver GPS, LoRa recognition and BW/SF inference, physical packet decoding and key-scoped likely Meshtastic classification. Remove interpretation, display, search, retention and export of text messages, sender/node identities, packet/request/reply IDs, sender-reported positions, telemetry and routes.

The Meshtastic path parses only a bounded Data envelope after eligible explicit-key processing. It skips the inner application payload and emits categorical status plus optional numeric port and signature presence. AES-CTR is unauthenticated: envelope plausibility can yield false positives, and neither a PHY CRC nor signature presence authenticates a sender.

## Saved recordings and reports

Existing source files remain unchanged. All historical reads and exports use allowlisted RF/GPS/classification projections. New files carry a validated metadata-only policy marker and keep legacy semantic columns NULL and route tables empty. Save copy reconstructs new-policy files from allowed records into a fresh database. It refuses unmarked historical files before creating an output, because a database-page copy could preserve old private contents. RF metadata exports remain available.

Reports retain independent receiver-GPS and operator-provenance controls. There is no content report or content opt-in; removed CLI options fail explicitly. Operator notes and receiver locations remain sensitive, and local files are not encrypted at rest.

## Remaining review and validation

Use protocol, legacy/new storage, UI, CLI, report and Save-copy exclusion tests to verify this boundary. Historical screenshots and content-decoding validation records are explicitly marked historical. Synthetic tests cannot establish field classification accuracy or exhaustiveness.
