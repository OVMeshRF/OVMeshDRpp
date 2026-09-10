# Documentation

OVMeshDR++ is an experimental receive-only RF survey application. Start with the user guide and current capability summary; measurement and protocol references provide the details needed to interpret or extend it.

## Using the application

- [User guide](user-guide.md): fresh sessions, receiver setup, GPS, recording, analysis and exports.
- [Receiver and platform compatibility](operations/hardware-compatibility.md): HackRF One and RTL-SDR capabilities, limits and validation status.
- [RTL-SDR setup](operations/rtl-sdr.md): source selection, sample rates, tuner gain and USB troubleshooting.
- [Build and packaging](operations/deployment.md): local builds, dependencies, storage locations and platform limits.
- [Screenshot gallery](screenshots/README.md): current interface examples and separately labeled sample reports.
- [Support playbook](operations/support-playbook.md): troubleshooting and useful diagnostic information.
- [Current capabilities and limitations](engineering/implementation-status.md).

## Understanding the data

- [Spectrum measurement specification](design/spectrum-measurement-record.md): frequency bins, thresholds, exposure, occupancy, power, center-guard comparisons and uncertainty.
- [Survey methodology](design/survey-and-analysis.md): planning observations and comparing frequency, time and receiver location.
- [Data model and retention](design/data-model-and-retention.md): recording schemas, compact storage, GPS association and export boundaries.
- [RF capability gaps](engineering/rf-survey-gap-analysis.md): what the results support and what still needs validation.
- [Validation scope](engineering/quality-and-validation.md): existing test coverage, known limitations and qualification gates.

## Developing and maintaining the project

- [Architecture](design/architecture.md) and [Meshtastic wire reference](research/meshtastic-wire-baseline.md).
- [Dependencies and licensing](engineering/dependencies-and-licensing.md), [third-party notices](../THIRD_PARTY_NOTICES.md), [OpenSSL intake](security/openssl-intake.md) and [Nanopb intake](security/nanopb-intake.md).
- [Security and privacy](security/security-and-privacy.md) and [test-data provenance](../tests/README.md).
- [Roadmap](product/roadmap.md), [development and releases](engineering/development-and-releases.md), and [source-release checklist](operations/github-publication.md).
- [Governance](governance/project-governance.md) and [documentation policy](governance/documentation-policy.md).

Join the [OVMesh Discord](https://discord.gg/kwKhFamfaU) for community questions and discussion.

Use [Contributing](../CONTRIBUTING.md), [Support](../SUPPORT.md), [Security](../SECURITY.md) and [Community conduct](../CODE_OF_CONDUCT.md) for participation and reporting.

Implemented behavior, synthetic correctness checks and field qualification are separate. A report or screenshot is not proof of calibrated measurements, exhaustive traffic detection, transmitter identity or regulatory compliance.

RTL-SDR [driver intake and license provenance](security/rtlsdr-intake.md) documents the reviewed source and local changes.
