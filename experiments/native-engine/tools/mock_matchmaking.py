#!/usr/bin/env python3
"""A stand-in for the matchmaking service, for tests on the loopback interface only.

Speaks just enough of the service's WebSocket protocol (as port/runtime/hle/slippi_matchmaking_ws.cpp
and tools/test_matchmaking_ws.cpp describe it) to pair the first two clients that join: each gets
"joined", then a "match" naming both players at 127.0.0.1 and the UDP port each one reported.
No TLS, no accounts, nothing leaves the machine. Point a services build at it with the app's own
override:  SLIPPI_MM_URL=ws://127.0.0.1:PORT

usage: mock_matchmaking.py [PORT]   (default 8765)
       mock_matchmaking.py PORT --dead-first N
           keeps running: each client's first N searches (counted by connect code) are matched at once with a
           made-up opponent at 127.0.0.1:9, where nothing answers, so that connection attempt fails after the
           netplay client's 8 s and the client searches again; after that, clients are paired with each other.
           For the failed-connection-then-search-again path (tests only).
       options for the pairing (either form):
         --dead-lan   every player's LAN address (ipAddressLan) is 127.0.0.1:9, where nothing answers. All players share the
                      public address 127.0.0.1, so a client dials the LAN address first (same router) and connects only by
                      falling back to the public one (patches/net-0006).
         --chat       each player carries sixteen custom quick-chat lines in the ticket (chatMessages), "P1 line 1".."P2 line 16".
         --ranks      each player carries Slippi 3.7.0 rank values (rank: rating, updateCount, placements, mmr); the second
                      player is a bot account (isBot), as the game records them in the replay."""
# SPDX-License-Identifier: GPL-3.0-or-later
import base64, hashlib, json, socket, struct, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 8765
dead_lan, with_chat, with_ranks = '--dead-lan' in sys.argv, '--chat' in sys.argv, '--ranks' in sys.argv
dead_first = int(sys.argv[sys.argv.index('--dead-first') + 1]) if '--dead-first' in sys.argv else None
server = socket.socket(); server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', port)); server.listen(4)

def read_exact(conn, n):
    data = b''
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk: raise ConnectionError('closed')
        data += chunk
    return data

def handshake(conn):
    request = b''
    while b'\r\n\r\n' not in request: request += conn.recv(4096)
    key = [l.split(b':', 1)[1].strip() for l in request.split(b'\r\n') if l.lower().startswith(b'sec-websocket-key')][0]
    accept = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
    conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')

def read_text(conn):
    text = b''
    while True:
        head = read_exact(conn, 2)
        opcode, length = head[0] & 0x0f, head[1] & 0x7f
        if length == 126: length = struct.unpack('>H', read_exact(conn, 2))[0]
        elif length == 127: length = struct.unpack('>Q', read_exact(conn, 8))[0]
        mask = read_exact(conn, 4) if head[1] & 0x80 else b'\0\0\0\0'
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(read_exact(conn, length)))
        if opcode == 0x9: send(conn, payload, 0xA); continue          # ping -> pong
        if opcode == 0x8: raise ConnectionError('client closed')
        if opcode in (0x1, 0x0): text += payload
        if head[0] & 0x80 and opcode in (0x1, 0x0): return text.decode()

def send(conn, payload, opcode=0x1):
    if isinstance(payload, str): payload = payload.encode()
    head = bytes([0x80 | opcode]) + (bytes([len(payload)]) if len(payload) < 126 else b'\x7e' + struct.pack('>H', len(payload)))
    conn.sendall(head + payload)

def extras(player, k):
    """The pairing options' fields for the player in port k + 1."""
    if dead_lan: player['ipAddressLan'] = '127.0.0.1:9'
    if with_chat: player['chatMessages'] = ['P%d line %d' % (k + 1, n + 1) for n in range(16)]
    if with_ranks:
        player['rank'] = {'rating': 1500.25 + 100 * k, 'updateCount': 12 + k, 'globalPlacement': 0, 'regionalPlacement': 7 * k, 'mmr': 1234.5 + k}
        player['isBot'] = k == 1
    return player

def match_message(mode, me, entries, host_index):
    """entries: (uid, displayName, connectCode, 'ip:port') per player, in port order."""
    players = [extras({'uid': uid, 'displayName': name, 'connectCode': code, 'port': k + 1, 'isLocalPlayer': k == me,
                       'ipAddress': address, 'ipAddressLan': address}, k) for k, (uid, name, code, address) in enumerate(entries)]
    return json.dumps({'type': 'match', 'matchId': 'mode.%s-mock' % mode, 'isHost': me == host_index, 'players': players,
                       'stages': [2, 3, 8, 28, 31, 32]})

def drain_and_close(conn):
    try: read_text(conn)
    except Exception: pass
    conn.close()

if dead_first is not None:
    import threading
    lock = threading.Lock(); searches = {}; waiting = []; matches = [0]
    def serve(conn):
        try:
            handshake(conn)
            join = json.loads(read_text(conn))
        except Exception:
            conn.close(); return
        user = join['user']; code = user.get('connectCode')
        mode = {0: 'ranked', 1: 'unranked', 2: 'direct', 3: 'teams', 4: 'party'}.get(join['search'].get('mode'), 'unranked')
        me_entry = (user['uid'], user['displayName'], code, '127.0.0.1:%d' % join['port'])
        with lock:
            searches[code] = searches.get(code, 0) + 1; n = searches[code]
        send(conn, json.dumps({'type': 'joined', 'ticketId': 't-%s-%d' % (code, n), 'token': 'mock'}))
        if n <= dead_first:
            # Alternate the sides so both roles (decider and not) fail: odd searches put the client first and make it the host.
            dead = ('mock-dead', 'Nobody', 'DEAD#000', '127.0.0.1:9')
            first = n % 2 == 1
            entries = [me_entry, dead] if first else [dead, me_entry]
            send(conn, match_message(mode, 0 if first else 1, entries, 0))
            print('mock: search %d of %s matched with a peer that never answers' % (n, code), flush=True)
            drain_and_close(conn); return
        with lock:
            waiting.append((conn, me_entry, mode))
            if len(waiting) < 2: return
            pair = waiting[:2]; del waiting[:2]; matches[0] += 1
        entries = [pair[0][1], pair[1][1]]
        for me, (c, _, _) in enumerate(pair):
            send(c, match_message(pair[0][2], me, entries, 0))
        print('mock: matched %s and %s in mode %s (pair %d)' % (entries[0][2], entries[1][2], pair[0][2], matches[0]), flush=True)
        for c, _, _ in pair: threading.Thread(target=drain_and_close, args=(c,), daemon=True).start()
    while True:
        conn, _ = server.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()

# While a ticket waits for its pair, a status message every two seconds, as the service sends them: the client counts a
# connection silent for 15 s as lost (slippi_matchmaking_ws.cpp kLivenessTimeout), so a long wait needs them.
import threading
send_lock = threading.Lock(); paired = threading.Event()
def keep_alive(conn):
    while not paired.wait(2.0):
        try:
            with send_lock: send(conn, json.dumps({'type': 'status'}))
        except Exception: return
clients = []
while len(clients) < 2:
    conn, _ = server.accept()
    handshake(conn)
    join = json.loads(read_text(conn))
    if 'user' not in join:          # a resume or anything else: this stand-in has no tickets to resume
        print('mock: %s message on a new connection, closed' % join.get('type'), flush=True); conn.close(); continue
    print('mock: join from %s mode %s port %s appVersion %s' % (join['user'].get('connectCode'), join['search'].get('mode'), join.get('port'), join.get('appVersion')), flush=True)
    send(conn, json.dumps({'type': 'joined', 'ticketId': 't-%d' % len(clients), 'token': 'mock'}))
    # The same player searching again (it cancelled and started over) replaces its earlier ticket.
    for k, (old_conn, old_join) in enumerate(clients):
        if old_join['user'].get('connectCode') == join['user'].get('connectCode'):
            print('mock: %s searched again; its earlier ticket is dropped' % join['user'].get('connectCode'), flush=True)
            old_conn.close(); del clients[k]; break
    clients.append((conn, join))
    threading.Thread(target=keep_alive, args=(conn,), daemon=True).start()
paired.set()
mode = {0: 'ranked', 1: 'unranked', 2: 'direct', 3: 'teams', 4: 'party'}.get(clients[0][1]['search'].get('mode'), 'unranked')
for me, (conn, _) in enumerate(clients):
    players = []
    for k, (_, join) in enumerate(clients):
        address = '127.0.0.1:%d' % join['port']
        players.append(extras({'uid': join['user']['uid'], 'displayName': join['user']['displayName'], 'connectCode': join['user']['connectCode'],
                               'port': k + 1, 'isLocalPlayer': k == me, 'ipAddress': address, 'ipAddressLan': address}, k))
    with send_lock:
        send(conn, json.dumps({'type': 'match', 'matchId': 'mode.%s-mock' % mode, 'isHost': me == 0, 'players': players,
                               'stages': [2, 3, 8, 28, 31, 32]}))
print('mock: matched two players in mode %s%s%s%s' % (mode, ' (LAN addresses that never answer)' if dead_lan else '',
      ', custom chat lines' if with_chat else '', ', ranks and a bot flag' if with_ranks else ''), flush=True)
for conn, _ in clients:
    try: read_text(conn)
    except Exception: pass
    conn.close()
