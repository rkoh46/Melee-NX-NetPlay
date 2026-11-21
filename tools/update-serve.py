#!/usr/bin/env python3
"""Serve the folder tools/update-host.py publishes to your console, and nothing else.

    python3 tools/update-serve.py --bind 192.168.1.20 --port 8080

Answers GET and HEAD for /latest.json and for the NRO files that manifest names. Every other path is
404: no directory listing, no other file of the folder, no path tricks.

The NROs contain the game built from your disc. Bind this to your computer's address on your HOME
NETWORK only, never to a public address and never behind a port forward. The server refuses addresses
that are not private ones; --all-interfaces (0.0.0.0) is for a computer whose only network is the home one.
There is no encryption and no login: anyone on that network can download the files.
"""
import argparse
import ipaddress
import json
import os
import shutil
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DEFAULT_PUBLISH = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "melee-nx-netplay/updates"


def allowed_files(publish_dir):
    """latest.json plus exactly the names it lists, when it is a manifest this tool understands."""
    names = set()
    try:
        manifest = json.loads((publish_dir / "latest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return names
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or not isinstance(manifest.get("files"), dict):
        return names
    names.add("latest.json")
    for entry in manifest["files"].values():
        name = entry.get("name") if isinstance(entry, dict) else None
        if isinstance(name, str) and name.endswith(".nro") and "/" not in name and "\\" not in name and not name.startswith("."):
            names.add(name)
    return names


def make_handler(publish_dir):
    class Handler(BaseHTTPRequestHandler):
        server_version = "melee-nx-update"
        sys_version = ""
        protocol_version = "HTTP/1.1"

        def resolve(self):
            path = self.path.split("?", 1)[0].split("#", 1)[0]
            if not path.startswith("/") or "/" in path[1:] or "%" in path or "\\" in path:
                return None
            name = path[1:]
            if name not in allowed_files(publish_dir):
                return None
            target = publish_dir / name
            return target if target.is_file() and not target.is_symlink() else None

        def respond(self, send_body):
            target = self.resolve()
            if target is None:
                body = b"not found\n"
                self.send_response(404)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if send_body:
                    self.wfile.write(body)
                return
            try:
                stream = open(target, "rb")
            except OSError:
                self.send_error(404)
                return
            with stream:
                size = os.fstat(stream.fileno()).st_size
                self.send_response(200)
                self.send_header("Content-Type", "application/json" if target.name.endswith(".json") else "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                if send_body:
                    shutil.copyfileobj(stream, self.wfile, 1 << 16)

        def do_GET(self):
            self.respond(True)

        def do_HEAD(self):
            self.respond(False)

        def log_message(self, format, *args):   # one line per request, for the service log
            sys.stderr.write("%s %s\n" % (self.address_string(), format % args))

    return Handler


def check_bind(address, all_interfaces):
    """None when the address may be served on, else the reason it may not."""
    try:
        ip = ipaddress.ip_address(address)
    except ValueError:
        return "%r is not an IP address" % address
    if ip.is_unspecified:
        return None if all_interfaces else "binding every interface needs --all-interfaces (only on a computer whose only network is your home one)"
    if ip.is_loopback or ip.is_private or ip.is_link_local:
        return None
    return "%s is a public address; the NROs contain the game and must stay on your home network" % address


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bind", required=True, help="this computer's address on your home network, e.g. 192.168.1.20")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--publish", type=Path, default=DEFAULT_PUBLISH, help="folder update-host.py publishes into (default: %(default)s)")
    parser.add_argument("--all-interfaces", action="store_true", help="allow --bind 0.0.0.0")
    args = parser.parse_args()
    reason = check_bind(args.bind, args.all_interfaces)
    if reason:
        print("update-serve: " + reason, file=sys.stderr)
        return 2
    publish_dir = args.publish.expanduser().resolve()
    publish_dir.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(publish_dir))
    print("update-serve: serving %s on http://%s:%d (home network only)" % (publish_dir, args.bind, server.server_address[1]), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
