# Source-release checklist

Experimental source is published at [OVMeshRF/OVMeshDRpp](https://github.com/OVMeshRF/OVMeshDRpp). The application display name is OVMeshDR++. This checklist applies to source updates and source packages; a supported binary release has additional [release-engineering requirements](../engineering/development-and-releases.md).

## Documentation and distribution contents

| Area | Required material |
| --- | --- |
| Introduction and changes | Current README, concise changelog, implementation status and known limits |
| Licensing and attribution | Project license, notices, component licenses, source/version provenance and applicable Corresponding Source |
| Build and operation | Reproducible build instructions, dependency inputs, user guide and platform limitations |
| Contributions and support | Contribution/conduct guidance, issue templates and realistic support expectations |
| Security and privacy | Private reporting route, privacy review and reviewed data-handling boundaries |
| Evidence | Requirements, decisions, validation records and clearly distinguished measured versus synthetic results |
| Images and fixtures | Reviewed screenshots, metadata and provenance; public or synthetic test fixtures |

Preserve upstream notices and meaningful technical validation. Do not present unbuilt-package checksums, an incomplete dependency inventory or a test badge as release evidence. CI, integrations and automated dependency changes require review of their permissions and behavior before adoption.

## Before publishing a source update

1. Check the branch, working tree and intended destination. Keep unrelated changes out of the update and preserve existing work.
2. Review the exact staged filenames and content, including commit author/committer identity, line endings, generated files and new binary assets. Run relevant local tests and verify vendored source hashes.
3. Run `python3 tools/audit_publication.py` for the index. Use `--all-objects` when reviewing the history or repository contents intended for sharing. Exit 1 means findings require review; exit 2 means scanning failed. Inspect all findings before proceeding.
4. Exclude local assistant instructions and scope/approval history, private audit notes, credentials, preferences, operational databases, raw RF captures, GPS logs/routes, private messages and device identifiers. Check examples, documentation, screenshots, image metadata and diagnostic files as well as source. A Git ignore rule does not protect tracked or force-added files.
5. Review public/synthetic fixtures against [test-data provenance](../../tests/README.md). Numeric coordinates or cryptographic constants require context; a pattern match alone does not establish that a fixture is private or safe.
6. Verify local documentation links, screenshot captions, licensing and stated feature/platform support against current evidence. Clearly label historical results and current gaps.
7. Recheck the staged scope and commit only reviewed content. Publish only the intended source branch; avoid broad ref or tag pushes that could include unreviewed history.
8. Read back the published revision and compare its tree, paths and metadata with the reviewed source. Keep the verification record with the release preparation materials.

The scanner reports finding categories and locations, not matched secret values. It cannot detect every encoded secret, image detail or vulnerability. Visual/manual review remains necessary, including local instructions and operational history that ordinary secret patterns may not recognize.

## Preparing a source-only snapshot

Use the explicit reviewed path list in `tools/public-source.json`. Each listed file is part of the distribution boundary; adding a path requires content, provenance and privacy review. The exporter reads exact committed blobs and excludes working/untracked files and Git history. It rejects missing paths, symlinks, operational file types and existing output destinations. `.gitignore` and `export-ignore` are additional safeguards, not substitutes for this allowlist.

From the reviewed source checkout, use a new destination under an existing local parent directory:

```sh
python3 tools/prepare_public_source.py --ref HEAD --output ../ovmesh-public-source
```

Review the exported filenames and bytes, image content/metadata, synthetic fixture provenance, notices and documentation links. Build and run the relevant offline tests from that exported tree. Preserve the printed receipt separately from the source package. The exporter is a packaging boundary, not a secret detector or approval to publish.

For an initial repository, initialize a new independent Git repository inside the reviewed output. Configure the intended public author and GitHub no-reply address, inspect the staged contents, run `tools/audit_publication.py`, and create one root commit. Scan `--all-objects` after committing and verify that there are no inherited refs, tags, alternates or additional commits. Do not copy another repository's `.git` directory or use a clone, fork or mirror to produce a history-free starting point.

Keep a new destination private through the review and compare its remote tree and commit metadata to the inspected local snapshot. Changing visibility is a separate publication step. Source archives may subsequently be generated from that reviewed public tree with `git archive`; archive export rules alone do not curate a private development checkout.

## Reporting and repository settings

Enable and verify [GitHub private vulnerability reporting](https://github.com/OVMeshRF/OVMeshDRpp/security/advisories/new) when publishing the repository. Confirm the private route is available before directing reporters there. Community questions and support discussions use the [OVMesh Discord](https://discord.gg/kwKhFamfaU). Track non-sensitive bugs and feature requests in [GitHub issues](https://github.com/OVMeshRF/OVMeshDRpp/issues). No response-time guarantee is established.

Review secret-scanning/push-protection availability, branch controls and integration permissions as part of repository maintenance. Use least privilege and avoid introducing write-capable services, deployment tokens or self-hosted runners without a concrete reviewed need.

If sensitive data is discovered after publication, assess exposure, revoke or rotate any affected credentials, and coordinate removal from history and other copies. Deleting the current file alone does not remove earlier versions. See [GitHub's sensitive-data removal guidance](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository).

Publishing source does not publish local surveys, create a cloud service or qualify a binary for distribution. Those boundaries remain separate.
