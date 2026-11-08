"""Synthetic provenance regressions. No game, user profile, or network is used."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from verify_port_inputs import build_sources, sha, sys_file_hashes, verify

VERIFIER = Path(__file__).with_name("verify_port_inputs.py")


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="melee-input-verification-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "source"
        self.root.mkdir()
        for path in build_sources(self.root):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic source\n")
        self.generated = self.base / "generated-a"
        self.generated.mkdir()
        (self.generated / "guest_000.cpp").write_text("synthetic guest\n")
        self.fobj = self.base / "FObjHost.cpp"
        self.fobj.write_text("synthetic animation adapter\n")
        self.decomp = self.base / "decomp"
        self.decomp.mkdir()
        (self.decomp / "fobj.c").write_text("synthetic decomp\n")
        self.dol = self.base / "main.dol"
        self.dol.write_bytes(b"synthetic image")
        self.sys_dir = self.root / "port/slippi_sys"
        (self.sys_dir / "GameFiles").mkdir(parents=True)
        (self.sys_dir / "codehandler.bin").write_bytes(b"synthetic handler")
        (self.sys_dir / "GameFiles/fixture.dat").write_bytes(b"synthetic runtime asset")
        self.binary = self.base / "synthetic-binary"
        self.binary.write_bytes(b"not an executable")
        self.commands = self.base / "compile_commands.json"
        self.commands.write_text("[]\n")
        self.manifest = self.base / "port-inputs.json"
        self.data = {
            "schema": 1, "source_root": str(self.root), "generated_dir": str(self.generated),
            "build_sources_sha256": {str(path.relative_to(self.root)): sha(path) for path in build_sources(self.root)},
            "generated_sha256": {"guest_000.cpp": sha(self.generated / "guest_000.cpp")},
            "decomp_root": str(self.decomp), "decomp_pin": {"files_sha256": {"fobj.c": sha(self.decomp / "fobj.c")}},
            "dol": {"path": str(self.dol), "sha1": sha(self.dol, "sha1")},
            "fobj_generated": {"path": str(self.fobj), "sha256": sha(self.fobj)},
            "slippi_enabled": True, "slippi_sha256": {"port/slippi_sys/codehandler.bin": sha(self.sys_dir / "codehandler.bin")},
            "slippi_sys_dir": str(self.sys_dir), "slippi_sys_files_sha256": sys_file_hashes(self.sys_dir),
        }
        self.save()

    def save(self):
        self.manifest.write_text(json.dumps(self.data))

    def check(self, generated=None, fobj=None):
        return verify(self.manifest, generated or self.generated, self.root, self.decomp, fobj or self.fobj)

    def invoke(self, *extra):
        return subprocess.run([sys.executable, str(VERIFIER), "--manifest", str(self.manifest),
            "--source-root", str(self.root), "--generated-dir", str(self.generated),
            "--decomp-root", str(self.decomp), "--fobj", str(self.fobj),
            "--binary", str(self.binary), *extra], capture_output=True, text=True)

    def test_valid_inputs_and_vanilla_profile(self):
        self.check()
        self.data["slippi_enabled"] = False
        self.data["slippi_sha256"] = {}
        self.save()
        self.check()  # Vanilla generation still binds the complete runtime Sys tree.

    def test_two_tree_cmake_override(self):
        other = self.base / "generated-b"
        other.mkdir()
        (other / "guest_000.cpp").write_bytes((self.generated / "guest_000.cpp").read_bytes())
        with self.assertRaisesRegex(ValueError, "CMake generated directory differs"):
            self.check(generated=other)

    def test_generated_content_and_inventory(self):
        (self.generated / "guest_000.cpp").write_text("changed")
        with self.assertRaisesRegex(ValueError, "changed input"):
            self.check()
        self.data["generated_sha256"]["guest_000.cpp"] = sha(self.generated / "guest_000.cpp")
        self.save()
        (self.generated / "extra.cpp").write_text("unlisted")
        with self.assertRaisesRegex(ValueError, "inventory changed"):
            self.check()

    def test_source_drift(self):
        (self.root / "CMakeLists.txt").write_text("changed source")
        with self.assertRaisesRegex(ValueError, "changed input"):
            self.check()

    def test_runtime_sys_mutation_and_extra_file(self):
        asset = self.sys_dir / "GameFiles/fixture.dat"
        asset.write_text("changed asset")
        with self.assertRaisesRegex(ValueError, "changed input"):
            self.check()
        self.data["slippi_sys_files_sha256"]["GameFiles/fixture.dat"] = sha(asset)
        self.save()
        (self.sys_dir / "extra.bin").write_text("extra")
        with self.assertRaisesRegex(ValueError, "inventory/content changed"):
            self.check()

    def test_runtime_sys_symlink_even_with_equal_bytes(self):
        target = self.sys_dir / "copy.bin"
        target.symlink_to(self.sys_dir / "codehandler.bin")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.check()

    def test_fobj_binding_and_bytes(self):
        other = self.base / "OtherFObj.cpp"
        other.write_bytes(self.fobj.read_bytes())
        with self.assertRaisesRegex(ValueError, "CMake FObj output differs"):
            self.check(fobj=other)
        self.fobj.write_text("changed generated adapter")
        with self.assertRaisesRegex(ValueError, "FObjHost.cpp changed"):
            self.check()

    def test_missing_schema_fields_fail_closed(self):
        for key in ("schema", "source_root", "fobj_generated", "slippi_sys_files_sha256"):
            original = self.data.pop(key)
            self.save()
            result = self.invoke()
            self.assertNotEqual(result.returncode, 0, key)
            self.assertFalse(json.loads(self.binary.with_name(self.binary.name + ".verified.json").read_text())["verified"])
            self.data[key] = original

    def test_post_link_and_failure_invalidate_old_sidecar(self):
        result = self.invoke("--compile-commands", str(self.commands))
        self.assertEqual(result.returncode, 0, result.stderr)
        sidecar = self.binary.with_name(self.binary.name + ".verified.json")
        self.assertTrue(json.loads(sidecar.read_text())["verified"])
        (self.generated / "guest_000.cpp").write_text("concurrent change")
        result = self.invoke("--compile-commands", str(self.commands))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(json.loads(sidecar.read_text())["verified"])

    def test_prelink_invalidates_and_missing_commands_fail(self):
        self.assertEqual(self.invoke("--invalidate").returncode, 0)
        sidecar = self.binary.with_name(self.binary.name + ".verified.json")
        self.assertFalse(json.loads(sidecar.read_text())["verified"])
        result = self.invoke("--compile-commands", str(self.base / "absent.json"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("compile_commands.json is missing", result.stderr)
        self.assertFalse(json.loads(sidecar.read_text())["verified"])


if __name__ == "__main__":
    unittest.main()
