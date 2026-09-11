#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicitly build the reviewed libcrypto locally; no pip or system install.

Download only with --download. Otherwise use an already downloaded archive.
Normal application configuration/build never invokes this helper.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import tarfile
import tempfile
import urllib.parse
import urllib.request


REPO = Path(__file__).resolve().parents[1]
TARGETS = {
    ("Linux", "x86_64"): "linux-x86_64",
    ("Linux", "aarch64"): "linux-aarch64",
    ("Linux", "arm64"): "linux-aarch64",
    ("Darwin", "arm64"): "darwin64-arm64-cc",
    ("Darwin", "x86_64"): "darwin64-x86_64-cc",
}


def native_target():
    target = TARGETS.get((platform.system(), platform.machine().lower()))
    if not target:
        raise ValueError("Helper supports native Linux x86_64/aarch64 and macOS arm64/x86_64. "
                         "Other targets require the documented manual OpenSSL build.")
    return target


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_output_path(repo, path):
    """Refuse redirected or non-build destinations, including existing symlinks."""
    repo = repo.resolve()
    path = Path(os.path.abspath(path))
    if not path.is_relative_to(repo / "build") or path == repo / "build":
        raise ValueError("Output must be inside this checkout's build directory.")
    for part in (path, *path.parents):
        if part == repo:
            break
        if part.is_symlink():
            raise ValueError("Symlink output paths are not permitted.")
    return path


def verify_archive(path, entry):
    if path.stat().st_size != entry["bytes"] or sha256(path) != entry["sha256"]:
        raise ValueError("Archive does not match the reviewed size/SHA-256; nothing will be extracted or executed.")


def validated_members(archive, prefix):
    members = archive.getmembers()
    if len(members) > 20000 or sum(m.size for m in members) > 512 * 1024 * 1024:
        raise ValueError("Archive exceeds extraction limits.")
    seen = set()
    for member in members:
        path = PurePosixPath(member.name)
        if (path.is_absolute() or not path.parts or path.parts[0] != prefix or
                any(part in (".", "..") for part in path.parts) or
                "\\" in member.name or any(ord(c) < 32 for c in member.name) or
                not (member.isfile() or member.isdir()) or member.size < 0 or
                str(path) in seen):
            raise ValueError("Archive contains an unsafe, duplicate or unexpected entry.")
        seen.add(str(path))
    return members


def extract_verified(path, destination, prefix):
    """Fresh destination, regular files/directories only; never restore tar owners."""
    with tarfile.open(path, "r:gz") as archive:
        members = validated_members(archive, prefix)
        for member in members:
            target = destination.joinpath(*PurePosixPath(member.name).parts)
            if member.isdir():
                target.mkdir(mode=0o700, parents=True, exist_ok=True)
                continue
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with archive.extractfile(member) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o700 if member.mode & 0o111 else 0o600)
    return destination / prefix


def checked_https(url):
    parsed = urllib.parse.urlsplit(url)
    host = parsed.hostname or ""
    if (parsed.scheme != "https" or parsed.username or parsed.password or
            parsed.port not in (None, 443) or
            not (host == "github.com" or host.endswith(".githubusercontent.com"))):
        raise ValueError("Download must use the official HTTPS release or its asset CDN.")
    return url


class ReleaseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        checked_https(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(entry, destination):
    request = urllib.request.Request(checked_https(entry["url"]), headers={"User-Agent": "OVMeshDRpp-source-setup"})
    opener = urllib.request.build_opener(ReleaseRedirect())
    count = 0
    with opener.open(request, timeout=60) as response, destination.open("xb") as output:
        checked_https(response.geturl())
        while True:
            block = response.read(1024 * 1024)
            if not block:
                break
            count += len(block)
            if count > entry["bytes"]:
                raise ValueError("Download exceeds the reviewed archive size.")
            output.write(block)
    verify_archive(destination, entry)


def run(command, cwd, log):
    with log.open("ab") as output:
        subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, check=True)


def write_probe(repo, probe):
    probe.mkdir()
    shutil.copyfile(repo / "third_party/openssl/intake_smoke.c", probe / "intake_smoke.c")
    (probe / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(CryptoIntake C)\n'
        'find_package(Threads REQUIRED)\n'
        'include("${OVMESH_SOURCE_DIR}/cmake/ReviewedOpenSSL.cmake")\n'
        'add_executable(intake intake_smoke.c)\n'
        'target_link_libraries(intake PRIVATE OpenSSL::Crypto Threads::Threads)\n')


def bootstrap(repo, archive, allow_download, prefix, jobs):
    target = native_target()
    prefix = checked_output_path(repo, prefix)
    if prefix.exists():
        raise ValueError("Destination already exists; it was left unchanged. Try configuring the app with it, "
                         "or choose a new --prefix under build for an independent build.")
    for tool in ("perl", "make", "cmake"):
        if not shutil.which(tool):
            raise ValueError("Missing build tool: " + tool)
    perl_check = subprocess.run(["perl", "-MTime::Piece", "-e", "1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if perl_check.returncode:
        raise ValueError("Perl cannot load Time::Piece. Install its package using your distribution's "
                         "package manager (Fedora: perl-Time-Piece), then retry. No download or build was started.")
    manifest = json.loads((repo / "third_party/openssl-source.json").read_text())
    entry = next(item for item in manifest["files"] if item["name"].endswith(".tar.gz"))
    if archive is None and not allow_download:
        raise ValueError("Choose --archive PATH or explicitly permit --download.")
    if archive is not None:
        archive = archive.resolve(strict=True)
        verify_archive(archive, entry)
    work_parent = checked_output_path(repo, repo / "build/deps")
    work_parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="openssl-build-", dir=work_parent))
    log = work / "build.log"
    print("Build work directory:", work, flush=True)
    if archive is None:
        print("Downloading the pinned official release (SHA-256 verification required)...", flush=True)
        archive = work / entry["name"]
        download(entry, archive)
    print("Verifying and extracting reviewed source...", flush=True)
    source = extract_verified(archive, work, "openssl-" + manifest["version"])
    for name, expected in manifest["source_file_hashes"].items():
        if sha256(source / name) != expected:
            raise ValueError("Reviewed build-source hash mismatch: " + name)
    arguments = []
    for arg in manifest["build_configuration"]["configure_arguments"]:
        if arg.startswith("--prefix="):
            arg = "--prefix=" + str(prefix)
        elif arg.startswith("--openssldir="):
            arg = "--openssldir=" + str(prefix / "ssl")
        else:
            arg = arg.replace("${OPENSSL_TARGET}", target)
        if "${" in arg:
            raise ValueError("Unsupported unresolved build-manifest variable.")
        arguments.append(arg)
    print("Configuring", target, "and building static libcrypto; details in", log, flush=True)
    run(["perl", "Configure", *arguments], source, log)
    for make_target in manifest["build_configuration"]["targets"]:
        run(["make", "-j", str(jobs), make_target], source, log)
    staged = work / "staged"
    (staged / "include/openssl").mkdir(parents=True)
    (staged / "lib").mkdir()
    headers = list((source / "include/openssl").glob("*.h"))
    for header in headers:
        if header.is_symlink() or not header.is_file():
            raise ValueError("Unexpected generated header entry.")
        shutil.copyfile(header, staged / "include/openssl" / header.name)
    shutil.copyfile(source / "libcrypto.a", staged / "lib/libcrypto.a")
    shutil.copyfile(source / "LICENSE.txt", staged / "LICENSE.txt")
    for required in ("opensslv.h", "configuration.h", "crypto.h", "evp.h"):
        if not (staged / "include/openssl" / required).is_file():
            raise ValueError("Missing generated public header: " + required)
    # Reuse the application's prefix-only target; never mix system openssl.pc
    # dependencies with the reviewed no-zlib archive.
    probe = work / "probe"
    write_probe(repo, probe)
    print("Checking hardened headers, linking, and AES known-answer tests...", flush=True)
    run(["cmake", "-G", "Unix Makefiles", "-S", str(probe), "-B", str(probe / "build"),
         "-DOVMESH_SOURCE_DIR=" + str(repo),
         "-DOPENSSL_ROOT_DIR=" + str(staged),
         "-DOPENSSL_INCLUDE_DIR=" + str(staged / "include"),
         "-DOPENSSL_CRYPTO_LIBRARY=" + str(staged / "lib/libcrypto.a")], work, log)
    run(["cmake", "--build", str(probe / "build"), "--parallel", str(jobs)], work, log)
    run([str(probe / "build/intake")], work, log)
    # Rename only after successful validation. Never modify an existing prefix.
    prefix = checked_output_path(repo, prefix)
    if prefix.exists():
        raise ValueError("Destination appeared during the build; existing contents were left unchanged.")
    prefix.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    staged.rename(prefix)
    print("Local OpenSSL ready:", prefix, flush=True)
    print("No system libraries, USB devices or services were changed.", flush=True)
    return prefix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--download", action="store_true", help="Download only the pinned official HTTPS release")
    source.add_argument("--archive", type=Path, help="Use a local pinned archive without network access")
    parser.add_argument("--prefix", type=Path, default=REPO / "build/deps/openssl-3.5.8-local",
                        help="New destination inside this checkout's build directory")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    try:
        bootstrap(REPO, args.archive, args.download, args.prefix, args.jobs)
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, "OpenSSL setup failed: " + str(error) +
                    "\nExisting prefixes are unchanged. Build details, when created, remain under build/deps/openssl-build-*/build.log.\n")


if __name__ == "__main__":
    main()
