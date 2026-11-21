#!/usr/bin/env python3
"""Follow Slippi releases without ever reporting a version this build does not implement.

The Slippi release the port implements is pinned in tools/slippi_pin.json. This tool compares the
pin with a newer release of Slippi Dolphin (project-slippi/Ishiiruka) and its Rust extensions:

* If only the game side changed (Gecko codes, Sys game files, the version number), it copies the
  new Sys files into port/slippi_sys, refreshes the hashes that gate the build and moves the pin.
  The game must then be regenerated and rebuilt (tools/update-slippi.sh does that with your disc).
* If any file that defines what the host side must speak changed (the EXI device, the netplay wire
  format, matchmaking, the game-file loader, the Rust extension surface: the list is
  `host_protocol` in the pin), it changes nothing, prints which files changed and where their
  counterparts live in this tree, and exits with status 3. That release needs a port first.

Exit status: 0 up to date or updated, 3 a port is needed, 1 anything else.
Only public repositories are read (git over HTTPS and GitHub's release API); nothing is sent.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN_PATH = ROOT / "tools/slippi_pin.json"
MANIFEST_PATH = ROOT / "tools/slippi_sys_manifest.json"
SOURCE_PINS_PATH = ROOT / "tools/port_source_pins.json"
CACHE = ROOT / "build/slippi-upstream"
RUST_SUBMODULE = "Externals/SlippiRustExtensions"
VERSION_FILE = "Source/Core/Common/Version.cpp"


def git(repo, *args, check=True, binary=False):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=False)
    if check and result.returncode != 0:
        raise SystemExit("git %s failed in %s:\n%s" % (" ".join(args), repo, result.stderr.decode(errors="replace").strip()))
    return result.stdout if binary else result.stdout.decode(errors="replace")


def mirror(name, url):
    """A blob-less clone in build/: commits and trees are local, file contents are fetched on demand."""
    path = CACHE / name
    if not (path / "HEAD").exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print("fetching %s (history only, about a minute the first time)" % url, flush=True)
        subprocess.run(["git", "clone", "--quiet", "--bare", "--filter=blob:none", url, str(path)], check=True)
    else:
        git(path, "fetch", "--quiet", "--tags", "--force", "origin", "+refs/heads/*:refs/heads/*")
    return path


def latest_release_tag():
    request = urllib.request.Request("https://api.github.com/repos/project-slippi/Ishiiruka/releases/latest",
                                     headers={"Accept": "application/vnd.github+json", "User-Agent": "update-slippi"})
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.load(response)["tag_name"]


def version_key(text):
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", text)
    return tuple(int(part) for part in match.groups()) if match else None


def changed(repo, old, new, paths):
    """[(path, added, removed)] for the given files or folders between two commits."""
    out = git(repo, "diff", "--numstat", old, new, "--", *paths)
    rows = []
    for line in out.splitlines():
        added, removed, path = line.split("\t", 2)
        rows.append((path, added, removed))
    return rows


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tag", help="the Slippi Dolphin release to move to (default: the latest release)")
    parser.add_argument("--check", action="store_true", help="report only; change nothing")
    parser.add_argument("--ported", action="store_true",
                        help="developers: the host side has been ported to this release; adopt its files and move the pin "
                             "even though host protocol files changed")
    args = parser.parse_args()

    pin = json.loads(PIN_PATH.read_text(encoding="utf-8"))
    try:
        tag = args.tag or latest_release_tag()
    except Exception as error:   # offline, rate limited, ...
        print("Could not ask GitHub for the latest Slippi release (%s). Nothing was changed." % error, file=sys.stderr)
        return 1
    if version_key(tag) is None:
        print("%s is not a release tag of the form v1.2.3. Nothing was changed." % tag, file=sys.stderr)
        return 1
    print("This build implements Slippi %s (%s). Requested release: %s." % (pin["version"], pin["ishiiruka"]["tag"], tag))
    if tag == pin["ishiiruka"]["tag"]:
        print("Up to date.")
        return 0
    if version_key(tag) < version_key(pin["ishiiruka"]["tag"]) and not args.ported:
        print("%s is older than the pinned release. Nothing was changed." % tag)
        return 0

    dolphin = mirror("Ishiiruka.git", pin["ishiiruka"]["repo"])
    old_commit = pin["ishiiruka"]["commit"]
    new_commit = git(dolphin, "rev-parse", tag + "^{commit}").strip()
    rust = mirror("slippi-rust-extensions.git", pin["rust_extensions"]["repo"])
    old_rust = pin["rust_extensions"]["commit"]
    new_rust = git(dolphin, "ls-tree", new_commit, RUST_SUBMODULE).split()[2]

    version_source = git(dolphin, "show", "%s:%s" % (new_commit, VERSION_FILE))
    match = re.search(r'#define\s+SLIPPI_REV_STR\s+"([^"]+)"\s*//\s*netplay', version_source)
    if not match:
        print("Could not read the netplay version from %s at %s. Nothing was changed." % (VERSION_FILE, tag), file=sys.stderr)
        return 1
    new_version = match.group(1)
    if version_key(new_version) is None:
        print("Release %s reports version %r, which is not a release version. Nothing was changed." % (tag, new_version), file=sys.stderr)
        return 1

    protocol = pin["host_protocol"]
    host_changes = [("Slippi Dolphin", path, added, removed, protocol["ishiiruka"].get(path) or next(
                        (where for prefix, where in protocol["ishiiruka"].items() if path.startswith(prefix)), ""))
                    for path, added, removed in changed(dolphin, old_commit, new_commit, list(protocol["ishiiruka"]))]
    if new_rust != old_rust:
        for path, added, removed in changed(rust, old_rust, new_rust, list(protocol["rust_extensions"])):
            where = next((where for prefix, where in protocol["rust_extensions"].items() if path.startswith(prefix)), "")
            host_changes.append(("Rust extensions", path, added, removed, where))
    sys_from = pin["sys_files"]["from"]
    game_changes = changed(dolphin, old_commit, new_commit, [sys_from + "/" + path for path in pin["sys_files"]["paths"]])

    print("\nSlippi %s -> %s" % (pin["version"], new_version))
    print("Game side (Gecko codes and Sys game files): %d file(s) changed" % len(game_changes))
    for path, added, removed in game_changes:
        print("  %s  (+%s -%s)" % (path, added, removed))
    print("Host side (protocol): %d file(s) changed" % len(host_changes))
    for origin, path, added, removed, where in host_changes:
        print("  [%s] %s  (+%s -%s)\n      here: %s" % (origin, path, added, removed, where or "see BUILDING.md (Following Slippi releases)"))

    if host_changes and not args.ported:
        print("\nThis release changes what the host side must speak, so it needs a port before this build may report %s.\n"
              "Nothing was changed. Until the port lands, online play will be refused by Slippi's matchmaking;\n"
              "offline play is unaffected. Compare with:\n"
              "  git -C %s diff %s %s -- <path>\n"
              "  git -C %s diff %s %s -- <path>\n"
              "After porting, run: python3 tools/update-slippi.py --tag %s --ported"
              % (new_version, dolphin, old_commit[:10], new_commit[:10], rust, old_rust[:10], new_rust[:10], tag))
        return 3
    if args.check:
        print("\nNo host protocol change: this release can be adopted by regenerating and rebuilding the game."
              if not host_changes else "\n--ported would adopt this release.")
        return 0

    # Adopt: copy the Sys files of the release, exactly the inventory the pin names.
    sys_to = ROOT / pin["sys_files"]["to"]
    wanted = {}
    for entry in pin["sys_files"]["paths"]:
        listing = git(dolphin, "ls-tree", "-r", "--name-only", new_commit, "--", sys_from + "/" + entry).splitlines()
        if not listing:
            print("The release has no %s/%s. Nothing was changed." % (sys_from, entry), file=sys.stderr)
            return 1
        for path in listing:
            wanted[path[len(sys_from) + 1:]] = git(dolphin, "show", "%s:%s" % (new_commit, path), binary=True)
    kept = {"README.md"}
    for existing in sorted(path for path in sys_to.rglob("*") if path.is_file()):
        relative = existing.relative_to(sys_to).as_posix()
        if relative not in wanted and relative not in kept:
            existing.unlink()
            print("removed %s" % relative)
    for relative, data in sorted(wanted.items()):
        target = sys_to / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
            print("updated %s" % relative)

    hashes = {path.relative_to(sys_to).as_posix(): sha256(path.read_bytes()) for path in sorted(sys_to.rglob("*")) if path.is_file()}
    manifest = {"source": "project-slippi/Ishiiruka %s (%s), Data/Sys" % (tag, new_commit), "files_sha256": hashes}
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    source_pins = json.loads(SOURCE_PINS_PATH.read_text(encoding="utf-8"))
    for relative in source_pins["slippi"]["files_sha256"]:
        source_pins["slippi"]["files_sha256"][relative] = hashes[relative]
    SOURCE_PINS_PATH.write_text(json.dumps(source_pins, indent=2) + "\n", encoding="utf-8")

    pin["version"] = new_version
    pin["ishiiruka"]["tag"], pin["ishiiruka"]["commit"] = tag, new_commit
    pin["rust_extensions"]["commit"] = new_rust
    PIN_PATH.write_text(json.dumps(pin, indent=2) + "\n", encoding="utf-8")
    shutil.rmtree(ROOT / "build/game/manifests", ignore_errors=True)   # the old translation no longer matches its inputs
    print("\nNow implementing Slippi %s. Regenerate and rebuild the game and the Replays app:\n"
          "  bash tools/update-slippi.sh /path/to/GALE01.iso   (or: bash install.sh /path/to/GALE01.iso)" % new_version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
