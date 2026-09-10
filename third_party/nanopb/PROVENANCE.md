# Nanopb source provenance

Reviewed/adopted 2026-09-09. Upstream release `nanopb-0.4.9.2`, commit
`160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f` (annotated tag object
`4133c1d3631654e80ed9bd27139318fd79fc2528`).

- [Official release](https://github.com/nanopb/nanopb/releases/tag/nanopb-0.4.9.2)
- [Source archive](https://github.com/nanopb/nanopb/releases/download/nanopb-0.4.9.2/nanopb-0.4.9.2.tar.gz)
- Archive SHA-256, matched to the official release asset digest:
  `98b8cadce538f37230ca0d5d8796894e3067d58dd2fb2618e6712c7362bdd8bb`

Unmodified adopted runtime files: `pb.h`, `pb_common.h`, `pb_common.c`,
`pb_decode.h`, `pb_decode.c`. Unmodified generation files:
`generator/nanopb_generator.py`, `generator/proto/__init__.py`,
`generator/proto/_utils.py`, `generator/proto/nanopb.proto`.
`LICENSE.txt` preserves Petteri Aimonen's zlib license and attribution.
The encoder, upstream examples/tests, packaging, alternate backends, and build
scripts are not part of the adopted runtime. Project CMake supplies the fixed
decoder flags described in [security intake](../../docs/security/nanopb-intake.md).

All adopted upstream files are byte-identical to the verified archive. Generated
Meshtastic files live separately. Project-owned provenance and generation scripts
are marked by their distinct names and are not represented as upstream files.
The reviewed local inventory is `third_party/integrity.json`; its hashes detect
unreviewed local drift, not upstream authenticity or absence of vulnerabilities.
