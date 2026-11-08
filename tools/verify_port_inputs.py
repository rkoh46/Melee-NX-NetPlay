"""Enforce the generated-game input manifest before compilation and after link.

No game is executed. A successful post-link check writes a local identity sidecar;
the absence of that sidecar, or any mismatch, prevents build acceptance.
"""
import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path, algorithm="sha256"):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def build_sources(root=ROOT):
    """All project/runtime input code, excluding local game outputs and tests."""
    files = {root / "CMakeLists.txt", root / "port/CMakeLists.txt",
             root / "VERSION", root / "tools/port_source_pins.json",
             root / "tools/bootstrap_port.py", root / "tools/generate_fobj_host.py",
             root / "tools/verify_port_inputs.py", root / "port/recomp/hle_list.txt"}
    for directory in ("port/recomp", "port/runtime", "port/app", "port/third_party", "native"):
        for path in (root / directory).rglob("*"):
            if path.is_file() and path.suffix in (".py", ".h", ".hpp", ".c", ".cpp", ".mm", ".inl"):
                files.add(path)
    return sorted(files)


def source_hashes():
    return {str(path.relative_to(ROOT)): sha(path) for path in build_sources()}


def sys_file_hashes(root):
    paths = list(root.rglob("*"))
    if any(path.is_symlink() for path in paths):
        raise ValueError("Slippi Sys inventory contains a symlink")
    return {str(path.relative_to(root)): sha(path) for path in paths if path.is_file()}


def check_files(root, expected, failures):
    for relative, digest in expected.items():
        path = root / relative
        try:
            path.resolve(strict=True).relative_to(root.resolve(strict=True))
        except (OSError, ValueError):
            failures.append("missing or escaped input: " + str(path))
            continue
        if not path.is_file() or sha(path) != digest:
            failures.append("changed input: " + str(path))


def verify(manifest_path, generated_dir, source_root=ROOT, decomp_root=None, fobj=None):
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or "build_sources_sha256" not in data:
        raise ValueError("manifest predates build-time verification; rerun tools/bootstrap_port.py")
    source_root = source_root.resolve(strict=True)
    if Path(data["source_root"]).resolve(strict=True) != source_root:
        raise ValueError("CMake source root differs from the manifest source root")
    generated = generated_dir.resolve(strict=True)
    if Path(data["generated_dir"]).resolve(strict=True) != generated:
        raise ValueError("CMake generated directory differs from the manifest generated directory")
    if decomp_root and Path(data["decomp_root"]).resolve(strict=True) != decomp_root.resolve(strict=True):
        raise ValueError("CMake decomp root differs from the manifest decomp root")
    failures = []
    if fobj:
        if Path(data["fobj_generated"]["path"]).resolve(strict=True) != fobj.resolve(strict=True):
            raise ValueError("CMake FObj output differs from the manifest generated output")
    actual_fobj = fobj if fobj else Path(data["fobj_generated"]["path"])
    if sha(actual_fobj) != data["fobj_generated"]["sha256"]:
        failures.append("generated FObjHost.cpp changed")
    check_files(source_root, data["build_sources_sha256"], failures)
    current_names = {str(path.relative_to(source_root)) for path in build_sources(source_root)}
    if current_names != set(data["build_sources_sha256"]):
        failures.append("project source inventory changed")
    check_files(generated, data["generated_sha256"], failures)
    generated_names = {path.name for path in generated.iterdir() if path.is_file()}
    if generated_names != set(data["generated_sha256"]):
        failures.append("generated game file inventory changed")
    check_files(Path(data["decomp_root"]), data["decomp_pin"]["files_sha256"], failures)
    check_files(source_root, data["slippi_sha256"], failures)
    sys_dir = Path(data["slippi_sys_dir"])
    check_files(sys_dir, data["slippi_sys_files_sha256"], failures)
    if sys_file_hashes(sys_dir) != data["slippi_sys_files_sha256"]:
        failures.append("Slippi Sys file inventory/content changed")
    if sha(Path(data["dol"]["path"]), "sha1") != data["dol"]["sha1"]:
        failures.append("DOL identity changed")
    if failures:
        raise ValueError("build inputs changed; regenerate the manifest before rebuilding:\n" + "\n".join(failures[:25]))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--generated-dir", type=Path, required=True,
                        help="actual generated directory selected by CMake, independent of the manifest")
    parser.add_argument("--decomp-root", type=Path)
    parser.add_argument("--fobj", type=Path, help="actual generated animation adapter selected by CMake")
    parser.add_argument("--binary", type=Path, help="after link, verify and record this artifact's identity")
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument("--invalidate", action="store_true", help="before link, mark the old artifact sidecar unverified")
    args = parser.parse_args()
    try:
        if args.binary:
            # Mark the sidecar unverified before any operation that can fail.
            # The PRE_LINK invocation also does this before the linker can replace
            # an older binary. Historical checkpoint sidecars are left untouched.
            binary = args.binary.resolve()
            sidecar = binary.with_name(binary.name + ".verified.json")
            sidecar.write_text(json.dumps({
                "verified": False, "kind": "awaiting successful post-link input verification",
                "binary": str(binary), "input_manifest": str(args.manifest.resolve()),
            }, indent=2) + "\n", encoding="utf-8")
        elif args.invalidate:
            raise ValueError("--invalidate requires --binary")
        if args.invalidate:
            return 0
        manifest = args.manifest.resolve(strict=True)
        verify(manifest, args.generated_dir, args.source_root, args.decomp_root, args.fobj)
        manifest_sha = sha(manifest)
        if args.binary:
            binary = args.binary.resolve(strict=True)
            binary_sha = sha(binary)
            record = {
                "verified": True,
                "kind": "post-link input verification; not runtime/gameplay acceptance",
                "verified_utc": datetime.now(timezone.utc).isoformat(),
                "binary": str(binary), "binary_sha256": binary_sha,
                "input_manifest": str(manifest), "input_manifest_sha256": manifest_sha,
            }
            if args.compile_commands:
                if not args.compile_commands.is_file():
                    raise ValueError("compile_commands.json is missing; configure CMAKE_EXPORT_COMPILE_COMMANDS=ON")
                record["compile_commands_sha256"] = sha(args.compile_commands)
            # Recheck after artifact hashing, so a concurrent source edit cannot
            # be admitted merely because the first verification ran before it.
            verify(manifest, args.generated_dir, args.source_root, args.decomp_root, args.fobj)
            if sha(manifest) != manifest_sha:
                raise ValueError("input manifest changed during post-link verification")
            sidecar.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            print("verified artifact " + binary_sha + " against manifest " + manifest_sha)
        else:
            print("verified source/generated inputs against manifest " + manifest_sha)
    except (OSError, ValueError, KeyError) as error:
        print("port input verification failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
