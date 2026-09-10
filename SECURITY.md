# Security

## Current status

The project publishes experimental native application source. No supported binary release or vulnerability-response SLA is established. Security concerns should be reported privately using the route below. Source hosting does not create a cloud service in the application.

The [security and privacy design](docs/security/security-and-privacy.md) describes required and proposed controls. Implemented controls, checks and residual risks are distinguished in [dependency inventory](docs/engineering/dependencies-and-licensing.md), [implementation status](docs/engineering/implementation-status.md) and [validation evidence](docs/engineering/quality-and-validation.md).

## Reporting a concern

Use [GitHub private vulnerability reporting](https://github.com/OVMeshRF/OVMeshDRpp/security/advisories/new), available under **Security > Report a vulnerability**. Do not post vulnerability details, credentials or private survey data in a public issue or Discord channel. If the private reporting form is unavailable, retain sensitive details until a private route is available.

Include a sanitized description, affected document, component or application version, expected and observed behavior, and reproduction conditions where safe. Do not attach channel/private keys, raw IQ, ciphertext, undecoded payloads, private messages, or precise survey routes.

A reporter should not perform additional radio access, collect another party's traffic, or transmit packets merely to substantiate a report. The maintainer and owner must agree on any controlled reproduction.

## Response planning

Before a supported binary release, define supported versions, triage ownership, acknowledgment targets, private coordination, advisory publication, and update delivery. Track these in [operations and support](docs/operations/support-playbook.md) and the [project governance](docs/governance/project-governance.md). An enabled reporting form is not a guaranteed response time.

## Protecting contributions and releases

Follow the [source-release checklist](docs/operations/github-publication.md). Review both file contents and the history being shared. Exclude operational recordings, location data, credentials and private metadata; editing the latest file does not remove earlier copies. The offline audit script emits finding locations instead of secret values; pattern scans do not prove that arbitrary encoded data or images are safe.
