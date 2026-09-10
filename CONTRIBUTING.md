# Contributing

This is an experimental C++20 application, currently version 0.4.0. Use the [public repository](https://github.com/OVMeshRF/OVMeshDRpp), [issues](https://github.com/OVMeshRF/OVMeshDRpp/issues) and [pull requests](https://github.com/OVMeshRF/OVMeshDRpp/pulls) for non-sensitive contributions. Source availability does not imply a supported binary release or validated operation on every platform.

Start with the [user guide](docs/user-guide.md), [implementation status](docs/engineering/implementation-status.md), [requirements](docs/product/roadmap.md), [build instructions](docs/operations/deployment.md) and [documentation policy](docs/governance/documentation-policy.md). Follow [community conduct](CODE_OF_CONDUCT.md). Suspected vulnerabilities go through [SECURITY.md](SECURITY.md), not public issues.

## Development and review

1. Keep one logical scope per branch and preserve existing work.
2. Explain the problem and resulting behavior; update affected requirements, decisions and documentation.
3. Run relevant local tests. The build does not download dependencies. Synthetic tests do not establish live RF performance; use hardware only with its operator's authorization.
4. Review privacy, native library loading and dependency provenance. Discuss a new dependency before adopting it; pin approved sources and preserve upstream notices.
5. Use LF text and focused diffs. Exclude build output, preferences, survey databases, raw captures, private routes/messages, credentials and personal paths. Use [public or synthetic test data](tests/README.md).
6. Review staged contents and commit identity before sharing. `python3 tools/audit_publication.py` provides local pattern triage, not a security guarantee. [Publication instructions](docs/operations/github-publication.md) cover history and source-package review.

Keep the application receive-only. Changes to data retention, protocol attribution or device access require explicit design review. Share source and reviewed synthetic fixtures; operational recordings and private device data do not belong in a contribution.

## Licensing

Original project code uses GPL-3.0-or-later; the combined inventoried build uses GPL version 3. Third-party code retains its applicable grants and notices. Read [LICENSE](LICENSE), [NOTICE](NOTICE), [component credits](THIRD_PARTY_NOTICES.md) and the [license inventory](docs/engineering/dependencies-and-licensing.md).

Contribute only material you have the right to license under the terms applicable to the affected project files. Identify copied/adapted material and retain copyright, license and version provenance. No separate contributor license agreement or mandatory sign-off process has been adopted. A source repository alone does not satisfy every obligation for a separately distributed binary.
