#!/usr/bin/env python3
"""A stand-in for Slippi's HTTP services, for tests on the loopback interface only.

The Linux services build is pinned to it (SLIPPI_PIN_SERVICES_URL=http://127.0.0.1:8766, patches/net-0008), the way it
is pinned to tools/mock_matchmaking.py for the queue. Nothing leaves the machine and nothing here is an account.

  GET  /user/<uid>        the profile refresh (Slippi Dolphin's overwriteFromServer): a display name and connect code
                          that differ from the made-up user.json ("Mock 1 (refreshed)"), latestVersion 3.7.0 and sixteen
                          custom chat lines ("Mine 1".."Mine 16"); also the rank fetch (rank values)
  POST /graphql           reportOnlineGame: success, and an uploadUrl pointing back here (unless --no-upload);
                          reportOnlineMatchStatus: true; anything else: null
  PUT  /upload/<n>        the replay upload: gzip, unpacked and checked to be a Slippi replay ({U\\x03raw[$U#l...)

Every request is printed as one line (the report's isoHash, the upload's size and SHA-256). With --save DIR each unpacked
upload is written to DIR/upload-<n>.slp, so a test can compare it with the replay the program wrote.

usage: mock_slippi_http.py [PORT] [--save DIR] [--no-upload]   (default port 8766)"""
# SPDX-License-Identifier: GPL-3.0-or-later
import gzip, hashlib, json, os, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 8766
save = sys.argv[sys.argv.index('--save') + 1] if '--save' in sys.argv else None
upload = '--no-upload' not in sys.argv
lock = threading.Lock()
uploads = [0]


def say(text):
    print('mock http: ' + text, flush=True)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body):
        data = json.dumps(body).encode() if not isinstance(body, bytes) else body
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n else b''

    def do_GET(self):
        if not self.path.startswith('/user/'):
            say('GET %s: not found' % self.path); self.reply(404, {}); return
        uid = self.path[len('/user/'):].split('?')[0]
        n = 1 + (int(uid.rsplit('-', 1)[-1]) if uid.rsplit('-', 1)[-1].isdigit() else 0)
        say('profile of %s asked (%s)' % (uid, self.path.split('?')[1] if '?' in self.path else 'no fields'))
        self.reply(200, {'uid': uid, 'displayName': 'Mock %d (refreshed)' % n, 'connectCode': 'MOCK#%03d' % n, 'latestVersion': '3.7.0',
                         'chatMessages': ['Mine %d' % (k + 1) for k in range(16)],
                         'rank': {'ratingOrdinal': 1600.5, 'dailyGlobalPlacement': None, 'dailyRegionalPlacement': 3, 'ratingUpdateCount': 9}})

    def do_POST(self):
        raw = self.body()
        try:
            request = json.loads(raw)
        except ValueError:
            say('POST %s: not JSON' % self.path); self.reply(400, {}); return
        query = request.get('query', '')
        report = (request.get('variables') or {}).get('report') or {}
        if 'reportOnlineGame' in query:
            with lock:
                uploads[0] += 1; n = uploads[0]
            say('game report: mode %s, match %s, game %s, %s frames, stage %s, winner %s, isoHash %s, %d players' % (
                report.get('mode'), report.get('matchId'), report.get('gameIndex'), report.get('gameDurationFrames'), report.get('stageId'),
                report.get('winnerIdx'), report.get('isoHash') or '(none)', len(report.get('players') or [])))
            result = {'success': True, 'uploadUrl': 'http://127.0.0.1:%d/upload/%d' % (port, n) if upload else None}
            self.reply(200, {'data': {'reportOnlineGame': result}})
        elif 'reportOnlineMatchStatus' in query:
            say('match status %s for %s' % (report.get('status'), report.get('matchId')))
            self.reply(200, {'data': {'reportOnlineMatchStatus': True}})
        else:
            say('graphql query not handled: %s' % query[:60])
            self.reply(200, {'data': None})

    def do_PUT(self):
        data = self.body()
        encoding = self.headers.get('Content-Encoding', '')
        try:
            slp = gzip.decompress(data) if encoding == 'gzip' else data
        except OSError:
            say('upload %s: %d bytes, not gzip' % (self.path, len(data))); self.reply(400, {}); return
        ok = slp.startswith(b'{U\x03raw[$U#l')
        say('replay upload %s: %d bytes (%s), %d unpacked, %s, sha256 %s' % (self.path, len(data), encoding or 'no encoding', len(slp),
            'a Slippi replay' if ok else 'NOT a Slippi replay', hashlib.sha256(slp).hexdigest()[:16]))
        if save:
            os.makedirs(save, exist_ok=True)
            with open(os.path.join(save, 'upload-%s.slp' % self.path.rsplit('/', 1)[-1]), 'wb') as f:
                f.write(slp)
        self.send_response(200 if ok else 400)
        self.send_header('Content-Length', '0')
        self.end_headers()


server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
say('listening on 127.0.0.1:%d%s' % (port, '' if upload else ' (no upload requests)'))
server.serve_forever()
