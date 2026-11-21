#!/usr/bin/env python3
"""Keep a build of Melee-NX-NetPlay ready for your console on an always-on computer.

One run (meant for a timer, see builder/systemd/):
  1. git pull --ff-only: the newest Melee-NX-NetPlay source.
  2. tools/update-slippi.py: has Slippi released something newer? A release that only changed the game
     side is adopted here; one that needs a port changes nothing and is reported as pending.
  3. If the source, the Slippi pin or the Sys files changed since the published build, rebuild both
     apps from your disc (the same build install.sh does).
  4. Publish into a folder: the two NROs under names that carry their checksum, then latest.json,
     each written to a temporary name and renamed, the manifest last. A console never sees a manifest
     that names a half-written file. Older NROs are removed afterwards.

tools/update-serve.py serves that folder to the console (launcher.ini: update_host=http://address:port).
The NROs contain the game built from your disc: serve them on your home network only.

Exit status: 0 published or nothing to do, 3 a Slippi release is waiting for a port (the last good
build stays published), 1 failure (the last good build stays published). The last line is a summary.
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PUBLISH = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "melee-nx-netplay/updates"
NROS = {"game": ("dist/sd/switch/melee-nx-netplay/melee-nx-netplay.nro", "melee-nx-netplay"),
        "replays": ("dist/sd/switch/melee-nx-replays/melee-nx-replays.nro", "melee-nx-replays")}
# What tools/update-slippi.py changes when it adopts a release on this computer.
ADOPTED_PATHS = ["port/slippi_sys", "tools/slippi_pin.json", "tools/slippi_sys_manifest.json", "tools/port_source_pins.json"]


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write(path, data):
    """Write beside the target, flush to disk, then rename over it."""
    path = Path(path)
    handle, temporary = tempfile.mkstemp(prefix=".tmp-", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    except BaseException:
        if os.path.exists(temporary):
            os.unlink(temporary)
        raise


def atomic_copy(source, target):
    target = Path(target)
    handle, temporary = tempfile.mkstemp(prefix=".tmp-", dir=target.parent)
    try:
        with os.fdopen(handle, "wb") as out, open(source, "rb") as stream:
            shutil.copyfileobj(stream, out, 1 << 20)
            out.flush()
            os.fsync(out.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, target)
    except BaseException:
        if os.path.exists(temporary):
            os.unlink(temporary)
        raise


def read_manifest(publish_dir):
    try:
        manifest = json.loads((Path(publish_dir) / "latest.json").read_text(encoding="utf-8"))
        return manifest if isinstance(manifest, dict) and manifest.get("schema") == 1 else None
    except (OSError, ValueError):
        return None


def write_manifest(publish_dir, manifest):
    atomic_write(Path(publish_dir) / "latest.json", (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))


def published_files_present(publish_dir, manifest):
    if not manifest:
        return False
    for entry in manifest.get("files", {}).values():
        path = Path(publish_dir) / entry["name"]
        if not path.is_file() or path.stat().st_size != entry["size"]:
            return False
    return "game" in manifest.get("files", {})


def publish(publish_dir, nros, app_version, slippi_version, commit, inputs, pending=None, built=None):
    """NROs first (under checksum-carrying names), the manifest last, stale NROs removed after."""
    publish_dir = Path(publish_dir)
    publish_dir.mkdir(parents=True, exist_ok=True)
    files = {}
    for key, (source, stem) in nros.items():
        digest = sha256_file(source)
        name = "%s-%s.nro" % (stem, digest[:12])
        target = publish_dir / name
        if not (target.is_file() and target.stat().st_size == os.path.getsize(source) and sha256_file(target) == digest):
            atomic_copy(source, target)
        files[key] = {"name": name, "size": os.path.getsize(source), "sha256": digest}
    manifest = {
        "schema": 1,
        "app_version": app_version,
        "slippi_version": slippi_version,
        "built": built or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commit": commit,
        "files": files,
        "pending": pending,
        "inputs": inputs,
    }
    write_manifest(publish_dir, manifest)
    keep = {entry["name"] for entry in files.values()}
    for stale in publish_dir.glob("*.nro"):
        if stale.name not in keep:
            stale.unlink()
    for leftover in publish_dir.glob(".tmp-*"):
        if time.time() - leftover.stat().st_mtime > 3600:
            leftover.unlink()
    return manifest


def git(*args, check=True):
    result = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True)
    if check and result.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), result.stderr.decode(errors="replace").strip()))
    return result


def source_fingerprint():
    """Identifies what a build is made from: the commit plus every local change (an adopted Slippi release)."""
    digest = hashlib.sha256()
    digest.update(git("rev-parse", "HEAD").stdout)
    digest.update(git("diff", "HEAD", "--binary").stdout)
    for name in sorted(git("ls-files", "--others", "--exclude-standard").stdout.decode().splitlines()):
        path = ROOT / name
        if path.is_file():
            digest.update(name.encode() + b"\0" + sha256_file(path).encode())
    return digest.hexdigest()


def pull():
    first = git("pull", "--ff-only", "--quiet", check=False)
    if first.returncode == 0:
        return
    # A Slippi release adopted here earlier can stand in the way of the port that supersedes it.
    git("checkout", "--", *ADOPTED_PATHS, check=False)
    git("clean", "-fdq", "--", "port/slippi_sys", check=False)
    second = git("pull", "--ff-only", "--quiet", check=False)
    if second.returncode != 0:
        raise RuntimeError("git pull --ff-only failed: " + second.stderr.decode(errors="replace").strip().splitlines()[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("iso", type=Path, help="your Melee disc image (NTSC-U 1.02), as for install.sh")
    parser.add_argument("--publish", type=Path, default=DEFAULT_PUBLISH, help="folder to publish into (default: %(default)s)")
    parser.add_argument("--no-pull", action="store_true", help="build this checkout as it is")
    parser.add_argument("--no-slippi-check", action="store_true", help="do not look for a newer Slippi release")
    parser.add_argument("--force", action="store_true", help="rebuild even if nothing changed")
    args = parser.parse_args()
    publish_dir = args.publish.expanduser()
    publish_dir.mkdir(parents=True, exist_ok=True)
    if not args.iso.is_file():
        print("update-host: FAILED: no disc image at %s" % args.iso)
        return 1

    lock = open(publish_dir / ".lock", "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print("update-host: another run is still building; nothing done")
        return 0

    previous = read_manifest(publish_dir)
    serving = "still serving %s (Slippi %s)" % (previous["app_version"], previous["slippi_version"]) if previous else "nothing published yet"
    pending = previous.get("pending") if previous else None
    try:
        if not args.no_pull:
            pull()
        slippi_code = 0
        if not args.no_slippi_check:
            with tempfile.TemporaryDirectory() as scratch:
                status_file = Path(scratch) / "status.json"
                slippi_code = subprocess.run([sys.executable, str(ROOT / "tools/update-slippi.py"), "--status-file", str(status_file)],
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                status = json.loads(status_file.read_text(encoding="utf-8")) if status_file.is_file() else {}
            if slippi_code == 3:
                pending = {"slippi": status.get("latest", ""), "reason": "needs a port"}
            elif slippi_code == 0:
                pending = None
            # any other status (no network, GitHub's rate limit): keep what was known
        pin = json.loads((ROOT / "tools/slippi_pin.json").read_text(encoding="utf-8"))
        app_version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        inputs = source_fingerprint()
        commit = git("rev-parse", "HEAD").stdout.decode().strip()

        if not args.force and previous and previous.get("inputs") == inputs and published_files_present(publish_dir, previous):
            if previous.get("pending") != pending:
                previous["pending"] = pending
                write_manifest(publish_dir, previous)
            if pending:
                print("update-host: Slippi %s needs a port; %s" % (pending["slippi"], serving))
                return 3
            print("update-host: nothing to do; %s" % serving)
            return 0

        started = time.time()
        log_path = ROOT / "build/update-host.log"
        log_path.parent.mkdir(exist_ok=True)
        with open(log_path, "wb") as log:
            # MELEE_NX_UPDATE_BUILD replaces the build command (for testing this script without a 30 minute build).
            command = [os.environ["MELEE_NX_UPDATE_BUILD"]] if os.environ.get("MELEE_NX_UPDATE_BUILD") else ["bash", str(ROOT / "install.sh")]
            build = subprocess.run(command + [str(args.iso.resolve())], stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, env=dict(os.environ, MELEE_NX_HOME=str(ROOT), MELEE_NX_NO_PULL="1"))
        nros = {key: (ROOT / relative, stem) for key, (relative, stem) in NROS.items()}
        fresh = all(path.is_file() and path.stat().st_mtime >= started - 1 for path, _ in nros.values())
        if build.returncode != 0 or not fresh:
            raise RuntimeError("the build failed (log: %s)" % log_path)
        publish(publish_dir, nros, app_version, pin["version"], commit, inputs, pending)
    except Exception as error:   # the published folder is untouched: the console keeps the last good build
        print("update-host: FAILED: %s; %s" % (error, serving))
        return 1
    if pending:
        print("update-host: published %s (Slippi %s, commit %s); Slippi %s needs a port" % (app_version, pin["version"], commit[:10], pending["slippi"]))
        return 3
    print("update-host: published %s (Slippi %s, commit %s)" % (app_version, pin["version"], commit[:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
