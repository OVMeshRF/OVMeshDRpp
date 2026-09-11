# Development and release engineering

Status: version 0.4.0 experimental native implementation. C++20/CMake, owned DSP/GPS handling and pinned source inventories are in use. The [public source repository](https://github.com/OVMeshRF/OVMeshDRpp), a local macOS development app and a regression suite exist. There is no supported signed binary release or application auto-updater; public source availability does not establish CI or platform qualification.

## Development lifecycle

1. Resolve requirements and architectural decisions, then make one logical change on an appropriate branch.
2. Review dependency/source provenance and retain licenses; normal builds use existing inputs without downloads.
3. Keep builds and tests in ignored build directories, with operational data excluded from Git.
4. Run relevant checks and record actual results and limits. Device tests require separate authorization.
5. Update affected behavior, security, support and deployment documentation in the same change.
6. Review staged contents, secrets, Git identity and generated files before committing or proposing publication.

[Current build instructions](../operations/deployment.md), [dependency inventory](dependencies-and-licensing.md), [OpenSSL intake](../security/openssl-intake.md) and [UI validation](quality-and-validation.md) provide the concrete inputs and evidence. Reproducible instructions are not a claim of demonstrated bit-for-bit builds. The normal application does not require Python/protoc; optional generators and developer tests remain separate.

## Source publication

Follow the [GitHub preparation checklist](../operations/github-publication.md). Publish only a reviewed source snapshot or a separately approved sanitized history. Preserve existing local history and private recordings; never push all branches or mirror this checkout without a history/privacy review. No remote operation is authorized by preparing files.

A source release includes build inputs, tests, current documentation, copyright/license notices and provenance. It excludes local Git metadata, machine inventories, downloaded review copies, compiled output, operational data and private working instructions. Use the reviewed publication manifest and a single starting commit for an initial snapshot.

## Binary releases

A development source version is not a supported binary release. Before distributing executables, complete:

- A tested OS/architecture/receiver and driver matrix, known limitations and install/uninstall instructions.
- Exact dependency closure and matching Corresponding Source, including applicable LGPL replacement/relinking requirements, generated schemas, patches and build/install scripts.
- Clean-machine packaging tests, signatures/notarization where applicable, artifact hashes and a release-specific component inventory or SBOM.
- RF/endurance, GPS and protocol compatibility evidence appropriate to the claims, plus storage/preferences compatibility and rollback instructions.
- Named security/release/support ownership and realistic supported-version and response policies.

The current macOS development bundle still links host USB libraries. RAK builds must also include the matching `ovmesh-rak-worker` beside the main executable; source/license manifests cover its pinned HAL. Windows/Linux runtime acceptance, signing and release packaging remain unfinished. Windows RAK operation is not implemented. No signing key, certificate or release token belongs in Git. Platform signing/notarization uploads require explicit authorization.

## Updates and rollback

Updates are explicit reviewed project changes. There is no application auto-updater or floating startup dependency installation. Track fixes for copied code as well as linked libraries; do not present a pinned version as permanently secure.

Keep old session readers available and use isolated preferences with older binaries. New session writes do not migrate existing files. For code rollback, preserve current work and use an appropriate retained branch or separate checkout instead of destructive resets. See [build isolation and format compatibility](../operations/deployment.md#build-isolation-and-format-compatibility) before changing application versions.

Release ownership remains tracked in [governance](../governance/project-governance.md). Public GitHub source publication and a supported application release are separate decisions.
