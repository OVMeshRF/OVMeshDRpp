# Governance and maintenance

OVMeshDRpp is an experimental open-source project. Maintainers review contributions, product scope and release readiness. Supported release lifetimes, response commitments and specialist maintenance assignments are not established.

## Decisions and review

Contributors explain the problem, evidence and tradeoffs in an issue or pull request and update affected documentation. Changes to measurement semantics, authorized decoding, retention, device access or dependencies require explicit design and security consideration. A proposal remains planned work until implemented and validated within its stated scope.

Keep one logical scope per change and preserve unrelated work. The application remains receive-only. Hardware tests require the relevant equipment operator's permission; access to source code does not grant permission to collect or publish operational data.

## Maintenance responsibilities

| Area | Responsibility |
|---|---|
| Product and engineering | Current capability definitions, review of changes and compatibility evidence |
| Security/privacy | Private report triage, coordinated handling and tested data boundaries |
| Dependencies/licenses | Source provenance, vulnerability/update review and distribution obligations |
| Packaging | Platform dependency closure, signing, clean-machine tests and supported-version policy |
| Documentation/support | Accurate guides, practical troubleshooting and realistic response expectations |

Assignments and service targets should describe actual capacity. One maintainer may cover several areas; the documentation does not imply a staffed specialist team.

## Participation

[Contributing](../../CONTRIBUTING.md), [Support](../../SUPPORT.md), [Security](../../SECURITY.md) and [Community conduct](../../CODE_OF_CONDUCT.md) define participation and reporting routes. Do not put vulnerability details or private survey information in public issues.

Describe legitimate receiver and survey uses clearly. Successful parsing is not authenticated identity, and an RF survey does not certify device compliance or guarantee an interference-free channel.

## Lifecycle and publication

Maintain readable session formats, explicit compatibility boundaries and reviewed dependency updates. Establish release lifetime and end-of-support communication before labeling a version supported.

Public documentation should summarize current behavior, meaningful validation evidence and unresolved risks. Keep operational recordings, individual access approvals and private development narratives outside published source. Preserve necessary licenses and source provenance.

Source updates follow the [release checklist](../operations/github-publication.md); binaries additionally require [release-engineering checks](../engineering/development-and-releases.md). Source hosting does not create automatic uploads, telemetry or deployment services in the application.
