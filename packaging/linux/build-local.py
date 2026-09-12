#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build/test Ubuntu packages with reusable, repository-local Docker caches."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASES = {
    "amd64": "ubuntu@sha256:a61567bd31828687156d735ea8eb01ba4e37636e225dd6a48ba94136a70d9d61",
    "arm64": "ubuntu@sha256:ec0b1c9058e44c837a21c3f9d8a3d5e9aaa94ed28edceb18e154af5efecf0950",
}


def run(*args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def mount(path, target, readonly=True):
    # Docker's --mount parser treats commas as separators, not filename data.
    if "," in str(path):
        raise ValueError("Docker mount paths cannot contain commas")
    return ["--mount", f"type=bind,src={path},dst={target}" + (",readonly" if readonly else "")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=["amd64", "arm64", "all"], default="all")
    parser.add_argument("--inputs", type=Path, default=ROOT / "build/inputs")
    parser.add_argument("--refresh-images", action="store_true",
                        help="Explicitly refresh signed Ubuntu repository packages")
    args = parser.parse_args()
    if git("status", "--porcelain"):
        raise ValueError("Commit reviewed source changes before packaging")
    inputs = args.inputs.resolve(strict=True)
    # Recheck all archive pins, even when dependency build prefixes are cached.
    entries = [json.loads((ROOT / "third_party/openssl-source.json").read_text())["files"][0]]
    entries += json.loads((ROOT / "third_party/usb-source.json").read_text())["components"]
    for entry in entries:
        archive = inputs / entry["name"]
        if (archive.stat().st_size != entry["bytes"] or
                hashlib.sha256(archive.read_bytes()).hexdigest() != entry["sha256"]):
            raise ValueError("Pinned archive mismatch: " + entry["name"])
    cache = ROOT / "build/linux-package-cache"
    cache.mkdir(parents=True, exist_ok=True)
    # A second invocation must not change source while the first is compiling it.
    lock = cache / "runner.lock"
    lock.mkdir()
    try:
        build(args, inputs, cache)
    finally:
        lock.rmdir()


def build(args, inputs, cache):
    spec = importlib.util.spec_from_file_location("public_source", ROOT / "tools/prepare_public_source.py")
    exporter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(exporter)
    identity = {"commit": git("rev-parse", "HEAD"), "tree": git("rev-parse", "HEAD^{tree}")}
    # Export only committed allowlisted files. Keep mtimes for unchanged sources
    # so Ninja can reuse application objects between releases.
    source = cache / "source"
    source.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache, prefix="export-") as temp:
        exported = Path(temp) / "source"
        exporter.prepare(ROOT, "HEAD", exported)
        names = exporter.manifest_paths((exported / exporter.MANIFEST).read_bytes())
        old_manifest = source / exporter.MANIFEST
        old_names = exporter.manifest_paths(old_manifest.read_bytes()) if old_manifest.exists() else []
        for name in old_names:
            if name not in names:
                (source / name).unlink(missing_ok=True)
        for name in names:
            src, dst = exported / name, source / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.is_symlink():
                raise ValueError("Unexpected source-cache symlink")
            if not dst.exists() or dst.read_bytes() != src.read_bytes():
                shutil.copyfile(src, dst)
            dst.chmod(src.stat().st_mode & 0o777)
    (source / "source-identity.json").write_text(json.dumps(identity) + "\n")
    # The nested writable mount must already exist under the read-only source.
    (source / "build").mkdir(exist_ok=True)
    recipe = ROOT / "packaging/linux"
    architectures = list(BASES) if args.arch == "all" else [args.arch]
    for arch in architectures:
        recipe_hash = hashlib.sha256((recipe / "Dockerfile").read_bytes() + BASES[arch].encode()).hexdigest()[:16]
        tags = {target: f"ovmesh-linux-{target}:{arch}-{recipe_hash}" for target in ("runtime", "builder")}
        for target, tag in tags.items():
            found = subprocess.run(["docker", "image", "inspect", tag],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
            if not found or args.refresh_images:
                refresh = ["--pull", "--no-cache"] if args.refresh_images else []
                run("docker", "build", "--platform", f"linux/{arch}", "--target", target,
                    "--build-arg", "BASE_IMAGE=" + BASES[arch], "-t", tag, *refresh, recipe)
        directory = cache / arch
        directory.mkdir(exist_ok=True)
        for child in ("fixtures", "publication-review", "output", "evidence"):
            (directory / child).mkdir(exist_ok=True)
        uid, gid = os.getuid(), os.getgid()
        if uid == 0:
            raise ValueError("Run the build helper as a non-root user")
        common = ["docker", "run", "--rm", "--platform", f"linux/{arch}",
                  "--network", "none", "--security-opt", "no-new-privileges"]
        command = common + ["--read-only", "--tmpfs", "/tmp:rw,exec,nosuid,size=1g,mode=1777",
                  "--cap-drop", "ALL", "--user", f"{uid}:{gid}",
                  "--env", "HOME=/tmp", "--workdir", "/src"]
        command += mount(source, "/src") + mount(directory, "/src/build", False)
        command += mount(recipe, "/packaging") + mount(inputs, "/inputs")
        for path in ("fixtures", "publication-review"):
            command += ["--tmpfs", f"/src/build/{path}:rw,exec,nosuid,size=2g,uid={uid},gid={gid},mode=0700"]
        run(*command, tags["builder"], "sh", "-c",
            "sh /packaging/build.sh && sh /packaging/package.sh")
        # Installation/removal occurs only in disposable containers. No host
        # devices, home, socket, system directories or networking are passed in.
        for target, extra in (("runtime", []), ("builder", ["gui"])):
            with (directory / "evidence" / f"package-{target}.log").open("w") as log:
                run(*(common + mount(directory / "output", "/packages") + mount(recipe, "/packaging")),
                    tags[target], "sh", "/packaging/test-package.sh", *extra,
                    stdout=log, stderr=subprocess.STDOUT)
        print(f"PASS: {arch} package and checks: {directory / 'output'}", flush=True)
    print("Dependency prefixes, Docker images and incremental build objects retained.")


if __name__ == "__main__":
    main()
