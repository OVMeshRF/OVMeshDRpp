# Documentation policy

Documentation describes the current product, how to use and build it, how measurements should be interpreted, and the evidence and limits behind support claims. Keep one authoritative page for each subject and link it from the [documentation index](../README.md).

## Claims and evidence

Distinguish implemented behavior, previously measured results, tests of the current revision, proposals and unvalidated targets. A passing synthetic test does not establish field performance; a report or screenshot does not establish calibrated accuracy.

Preserve uncertainties, failures and known gaps in concise technical language. Correct errors and replace superseded guidance. Public documentation should not reproduce private conversations, personal hardware arrangements, individual approvals, operational routes or the chronology of development sessions.

## Updates with a change

Update the affected references in the same logical change:

- User guide and support instructions for workflow or default changes.
- Architecture and data model for processing interfaces or stored fields.
- Measurement specification and validation scope for estimator changes.
- Security/privacy and source provenance for changed trust boundaries or dependencies.
- Build, compatibility and release guidance for platform or packaging changes.
- Roadmap and a concise changelog for significant planned or delivered capabilities.

Create new documents only when they serve a concrete reader or maintenance need. Avoid duplicating status across overlapping planning documents.

## Validation records

Retain sufficient reproducibility information for technical results: source revision, method, toolchain/configuration, expected and observed outcomes, fixture provenance and limitations. Publish sanitized aggregate evidence and reviewed public/synthetic fixtures. Operational evidence and private review materials remain separate.

Link protocol and dependency behavior to original source and immutable versions where applicable. Keep provenance hashes and required third-party notices even when simplifying surrounding prose. A source's specification is distinct from this application's measured behavior.

## Review before publication

Check the exact outgoing file set and history, secrets/private data, image pixels and metadata, licenses, links, anchors, line endings and unsupported readiness claims. Exclusion rules help select files but do not replace inspection. Follow the [source-release checklist](../operations/github-publication.md).

Documentation-only edits ordinarily need link/content/privacy review rather than hardware access or a full application test run. Record tests actually performed and distinguish them from earlier results. Changes to estimator descriptions must agree with source behavior; wording must not hide an implementation gap.
