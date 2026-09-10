#!/usr/bin/env python3
"""Offline schema generation. Invoke with python3 -I -S; never downloads tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import runpy
import subprocess
import sys
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_zip(archive, destination, expected):
    if digest(archive) != expected:
        raise RuntimeError("generation archive checksum mismatch")
    with zipfile.ZipFile(archive) as source:
        expected_files = set()
        for item in source.infolist():
            path = PurePosixPath(item.filename)
            if path.is_absolute() or ".." in path.parts or ((item.external_attr >> 16) & 0o170000) == 0o120000:
                raise RuntimeError("unsafe generation archive member")
            if not item.is_dir():
                expected_files.add(Path(item.filename))
            for parent in [destination / item.filename] + list((destination / item.filename).parents):
                if parent.is_symlink():
                    raise RuntimeError("symbolic link in generation tool output")
        if destination.exists():
            actual_files = {path.relative_to(destination) for path in destination.rglob("*") if path.is_file()}
            if not actual_files.issubset(expected_files):
                raise RuntimeError("unexpected existing generation tool files")
        source.extractall(destination)


def main():
    if not sys.flags.isolated or not sys.flags.no_site:
        raise RuntimeError("invoke using python3 -I -S")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="compare regenerated C output without changing tracked files")
    parser.add_argument("--python-only", action="store_true", help="generate independent official Python schema modules only")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = root / "third_party/meshtastic"
    manifest = json.loads((root / "third_party/integrity.json").read_text())
    expected_inputs = set()
    for entry in manifest["files"]:
        if entry["path"].startswith(("third_party/nanopb/", "third_party/meshtastic/")):
            expected_inputs.add(entry["path"])
            path = root / entry["path"]
            if path.is_symlink() or digest(path) != entry["sha256"]:
                raise RuntimeError("review generation input changes before updating integrity manifest")
    actual_inputs = {str(path.relative_to(root)) for directory in (source, root / "third_party/nanopb")
                     for path in directory.rglob("*") if path.is_file()}
    if actual_inputs != expected_inputs:
        raise RuntimeError("review changed generation input inventory before continuing")
    work = root / "build/schema-tools"
    tool_manifest = json.loads((source / "toolchain.json").read_text())
    for tool in tool_manifest["archives"]:
        extract_zip(work / "downloads" / tool["filename"], work / tool["directory"], tool["sha256"])
    protoc = work / "protoc-36.1/bin/protoc"
    if digest(protoc) != tool_manifest["protoc_binary_sha256"]:
        raise RuntimeError("protoc binary checksum mismatch")
    protoc.chmod(0o755)
    scratch = work / "work"
    scratch.mkdir(parents=True, exist_ok=True)
    # Nanopb's fallback discovery/automatic protoc invocation is disabled.
    # Only the verified pure Python wheel and explicit generator paths are added.
    sys.dont_write_bytecode = True
    os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"
    os.environ["NANOPB_PB2_NO_REBUILD"] = "1"
    generator = root / "third_party/nanopb/generator"
    sys.path[:0] = [str(work / "python"), str(scratch), str(generator)]
    subprocess.run([str(protoc), "-I" + str(generator / "proto"),
                    "-I" + str(work / "protoc-36.1/include"),
                    "--python_out=" + str(scratch), str(generator / "proto/nanopb.proto")], check=True)
    for version in ("v2.7.19", "v2.8.0"):
        schemas = source / "schemas" / version
        output = work / "python-schemas" / version
        output.mkdir(parents=True, exist_ok=True)
        inputs = sorted(schemas.rglob("*.proto"))
        subprocess.run([str(protoc), "-I" + str(schemas), "--python_out=" + str(output)] +
                       [str(path) for path in inputs], check=True)
    if args.python_only:
        print("Generated independent official Python schema modules in build/schema-tools/python-schemas")
        return
    schemas = source / "schemas/v2.8.0"
    descriptors = []
    for path in sorted(schemas.rglob("*.proto")):
        output = scratch / (path.stem + ".pb")
        subprocess.run([str(protoc), "-I" + str(schemas), "--include_imports", "-o" + str(output), str(path)], check=True)
        descriptors.append(str(output))
    output = work / "check-generated" if args.check else source / "generated"
    output.mkdir(parents=True, exist_ok=True)
    sys.argv = ["nanopb_generator.py", "-I", str(source / "options"), "-D", str(output),
                "--error-on-unmatched"] + descriptors
    runpy.run_path(str(generator / "nanopb_generator.py"), run_name="__main__")
    for header in output.rglob("*.pb.h"):
        text = header.read_text()
        if "pb_callback_t" in text or ", POINTER," in text or ", CALLBACK," in text:
            raise RuntimeError("unexpected pointer/callback field in generated types")
    if args.check:
        expected = source / "generated"
        relative = sorted(path.relative_to(output) for path in output.rglob("*.pb.*"))
        if relative != sorted(path.relative_to(expected) for path in expected.rglob("*.pb.*")):
            raise RuntimeError("generated inventory mismatch")
        if any((output / path).read_bytes() != (expected / path).read_bytes() for path in relative):
            raise RuntimeError("generated output differs from reviewed files")
        print("Regeneration byte comparison passed")


if __name__ == "__main__":
    main()
