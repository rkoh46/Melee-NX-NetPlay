"""Host tests for the build computer's side of self-updating: the manifest writer (tools/update-host.py)
and the server (tools/update-serve.py). Run: python3 tools/test_update_host.py"""
import hashlib
import http.client
import importlib.util
import json
import os
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

TOOLS = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, TOOLS / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


host = load("update_host", "update-host.py")
serve = load("update_serve", "update-serve.py")


class PublishTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.root = Path(self.scratch.name)
        self.publish_dir = self.root / "updates"
        self.sources = self.root / "src"
        self.sources.mkdir()

    def tearDown(self):
        self.scratch.cleanup()

    def nros(self, game, replays):
        (self.sources / "game.nro").write_bytes(game)
        (self.sources / "replays.nro").write_bytes(replays)
        return {"game": (self.sources / "game.nro", "melee-nx-netplay"), "replays": (self.sources / "replays.nro", "melee-nx-replays")}

    def test_manifest_describes_the_files(self):
        game, replays = os.urandom(300_000), os.urandom(200_000)
        manifest = host.publish(self.publish_dir, self.nros(game, replays), "0.3.8", "3.7.0", "c" * 40, "fingerprint", built="2026-01-02T03:04:05Z")
        on_disk = json.loads((self.publish_dir / "latest.json").read_text())
        self.assertEqual(manifest, on_disk)
        self.assertEqual(on_disk["schema"], 1)
        self.assertEqual((on_disk["app_version"], on_disk["slippi_version"], on_disk["commit"], on_disk["built"]), ("0.3.8", "3.7.0", "c" * 40, "2026-01-02T03:04:05Z"))
        self.assertIsNone(on_disk["pending"])
        for key, data in (("game", game), ("replays", replays)):
            entry = on_disk["files"][key]
            digest = hashlib.sha256(data).hexdigest()
            self.assertEqual(entry["sha256"], digest)
            self.assertEqual(entry["size"], len(data))
            self.assertIn(digest[:12], entry["name"])
            self.assertEqual((self.publish_dir / entry["name"]).read_bytes(), data)
        self.assertTrue(host.published_files_present(self.publish_dir, on_disk))
        self.assertEqual(list(self.publish_dir.glob(".tmp-*")), [])

    def test_republish_replaces_and_removes_the_old_files(self):
        first = host.publish(self.publish_dir, self.nros(b"a" * 5000, b"b" * 5000), "0.3.8", "3.7.0", "1" * 40, "one")
        second = host.publish(self.publish_dir, self.nros(b"c" * 6000, b"b" * 5000), "0.3.9", "3.7.0", "2" * 40, "two",
                              pending={"slippi": "3.8.0", "reason": "needs a port"})
        self.assertNotEqual(first["files"]["game"]["name"], second["files"]["game"]["name"])
        self.assertEqual(first["files"]["replays"]["name"], second["files"]["replays"]["name"])   # unchanged file keeps its name
        self.assertEqual(sorted(p.name for p in self.publish_dir.glob("*.nro")), sorted(e["name"] for e in second["files"].values()))
        self.assertEqual(json.loads((self.publish_dir / "latest.json").read_text())["pending"], {"slippi": "3.8.0", "reason": "needs a port"})

    def test_a_missing_or_short_file_is_noticed(self):
        manifest = host.publish(self.publish_dir, self.nros(b"a" * 5000, b"b" * 5000), "0.3.8", "3.7.0", "1" * 40, "one")
        (self.publish_dir / manifest["files"]["game"]["name"]).write_bytes(b"short")
        self.assertFalse(host.published_files_present(self.publish_dir, manifest))
        self.assertFalse(host.published_files_present(self.publish_dir, None))

    def test_a_failed_copy_leaves_the_published_build_alone(self):
        manifest = host.publish(self.publish_dir, self.nros(b"a" * 5000, b"b" * 5000), "0.3.8", "3.7.0", "1" * 40, "one")
        before = (self.publish_dir / "latest.json").read_bytes()
        broken = {"game": (self.sources / "does-not-exist.nro", "melee-nx-netplay")}
        with self.assertRaises(OSError):
            host.publish(self.publish_dir, broken, "0.3.9", "3.7.0", "2" * 40, "two")
        self.assertEqual((self.publish_dir / "latest.json").read_bytes(), before)
        self.assertTrue(host.published_files_present(self.publish_dir, manifest))

    def test_unreadable_manifest_reads_as_none(self):
        self.publish_dir.mkdir()
        self.assertIsNone(host.read_manifest(self.publish_dir))
        (self.publish_dir / "latest.json").write_text("{not json")
        self.assertIsNone(host.read_manifest(self.publish_dir))
        (self.publish_dir / "latest.json").write_text('{"schema": 7}')
        self.assertIsNone(host.read_manifest(self.publish_dir))


class ServeTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.publish_dir = Path(self.scratch.name) / "updates"
        self.src = Path(self.scratch.name) / "src"
        self.src.mkdir()
        self.game = os.urandom(400_000)
        (self.src / "g.nro").write_bytes(self.game)
        (self.src / "r.nro").write_bytes(b"r" * 3000)
        self.manifest = host.publish(self.publish_dir, {"game": (self.src / "g.nro", "melee-nx-netplay"), "replays": (self.src / "r.nro", "melee-nx-replays")},
                                     "0.3.8", "3.7.0", "c" * 40, "inputs")
        (self.publish_dir / "secret.txt").write_text("not for the network")
        (self.publish_dir / "old-build.nro").write_bytes(b"unlisted")
        (Path(self.scratch.name) / "outside.txt").write_text("outside")
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), serve.make_handler(self.publish_dir.resolve()))
        self.server.RequestHandlerClass.log_message = lambda *a, **k: None
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.scratch.cleanup()

    def request(self, path, method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=10)
        connection.request(method, path)
        response = connection.getresponse()
        body = response.read()
        connection.close()
        return response.status, dict(response.getheaders()), body

    def test_serves_the_manifest_and_the_listed_nros(self):
        status, headers, body = self.request("/latest.json")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), self.manifest)
        name = self.manifest["files"]["game"]["name"]
        status, headers, body = self.request("/" + name)
        self.assertEqual((status, body), (200, self.game))
        self.assertEqual(int(headers["Content-Length"]), len(self.game))
        status, headers, body = self.request("/" + name, "HEAD")
        self.assertEqual((status, body, int(headers["Content-Length"])), (200, b"", len(self.game)))

    def test_everything_else_is_not_found(self):
        for path in ("/", "/secret.txt", "/old-build.nro", "/.lock", "/../outside.txt", "/%2e%2e/outside.txt", "/latest.json/", "/sub/latest.json",
                     "/..%2foutside.txt", "/LATEST.JSON", "/" + self.manifest["files"]["game"]["name"] + "/..", "/\\..\\outside.txt"):
            status, _, body = self.request(path)
            self.assertEqual(status, 404, path)
            self.assertNotIn(b"outside", body)
            self.assertNotIn(b"not for the network", body)
        self.assertEqual(self.request("/latest.json", "POST")[0], 501)

    def test_a_manifest_cannot_point_outside_the_folder(self):
        evil = dict(self.manifest, files={"game": {"name": "../outside.txt", "size": 7, "sha256": "0" * 64}, "x": {"name": "secret.txt", "size": 1, "sha256": "0" * 64}})
        (self.publish_dir / "latest.json").write_text(json.dumps(evil))
        self.assertEqual(serve.allowed_files(self.publish_dir), {"latest.json"})
        self.assertEqual(self.request("/secret.txt")[0], 404)

    def test_no_manifest_serves_nothing(self):
        (self.publish_dir / "latest.json").unlink()
        self.assertEqual(self.request("/latest.json")[0], 404)
        self.assertEqual(self.request("/" + self.manifest["files"]["game"]["name"])[0], 404)

    def test_bind_addresses(self):
        for good in ("192.168.1.20", "10.0.0.5", "172.16.3.4", "127.0.0.1", "fd00::1"):
            self.assertIsNone(serve.check_bind(good, False), good)
        for bad in ("8.8.8.8", "1.2.3.4", "2001:4860:4860::8888", "example.com", "0.0.0.0"):
            self.assertIsNotNone(serve.check_bind(bad, False), bad)
        self.assertIsNone(serve.check_bind("0.0.0.0", True))


if __name__ == "__main__":
    unittest.main()
