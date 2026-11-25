#!/usr/bin/env python3
"""A one-node BitTorrent mainline DHT (BEP 5) for the lobby test only: the local stand-in for the public DHT's entry points.

melee-unlocked's peer lobby (port/app/launcher_lobby_p2p.cpp, jech/dht) pings its entry point, asks it for nodes, then looks up
and announces its lobby topics there. This node answers ping, find_node, get_peers and announce_peer as BEP 5 says, keeps the
peers announced per info hash and hands out the nodes that have spoken to it, so two lobbies that only know this node find each
other through the DHT itself, exactly as through the public one. It never sends a query of its own and never contacts anything.

It computes the public lobby topic independently, as BLAKE2b-160 of "melee-unlocked-lobby-v1-public" (upstream's topic_hash:
crypto_blake2b with a 20-byte output), and logs when a lobby announces or looks it up, so the test shows that the Switch app
uses melee-unlocked's own info hash.

usage: dht_bootstrap.py HOST PORT        (the test binds the container's own address on an internal network: never public)"""
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib, hmac, os, socket, sys, time

PUBLIC_TOPIC = hashlib.blake2b(b"melee-unlocked-lobby-v1-public", digest_size=20).digest()

def bdecode(data, i=0):
    c = data[i:i + 1]
    if c == b"i":
        end = data.index(b"e", i)
        return int(data[i + 1:end]), end + 1
    if c == b"l":
        i += 1; out = []
        while data[i:i + 1] != b"e":
            v, i = bdecode(data, i); out.append(v)
        return out, i + 1
    if c == b"d":
        i += 1; out = {}
        while data[i:i + 1] != b"e":
            k, i = bdecode(data, i); v, i = bdecode(data, i); out[k] = v
        return out, i + 1
    colon = data.index(b":", i)
    n = int(data[i:colon])
    return data[colon + 1:colon + 1 + n], colon + 1 + n

def bencode(v):
    if isinstance(v, int): return b"i%de" % v
    if isinstance(v, bytes): return b"%d:%s" % (len(v), v)
    if isinstance(v, str): return bencode(v.encode())
    if isinstance(v, list): return b"l" + b"".join(bencode(x) for x in v) + b"e"
    if isinstance(v, dict): return b"d" + b"".join(bencode(k) + bencode(v[k]) for k in sorted(v)) + b"e"
    raise TypeError(type(v))

def compact(ip, port): return socket.inet_aton(ip) + port.to_bytes(2, "big")

def main():
    host, port = sys.argv[1], int(sys.argv[2])
    me = os.urandom(20); secret = os.urandom(16)
    nodes = {}          # (ip, port) -> (node id, last heard)
    peers = {}          # info hash -> {(ip, port): time}
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((host, port))
    print("dht: node on %s:%d; the public lobby topic is %s" % (host, port, PUBLIC_TOPIC.hex()), flush=True)
    def token(ip): return hmac.new(secret, ip.encode(), hashlib.sha1).digest()[:8]
    def closest(target, exclude):
        known = [(int.from_bytes(nid, "big") ^ int.from_bytes(target, "big"), nid, a) for a, (nid, _) in nodes.items() if a != exclude]
        return b"".join(nid + compact(*a) for _, nid, a in sorted(known)[:8])
    def topic(h): return "the public lobby topic" if h == PUBLIC_TOPIC else "topic " + h.hex()[:12]
    while True:
        data, addr = s.recvfrom(4096)
        try:
            msg, _ = bdecode(data)
        except Exception:
            continue
        if not isinstance(msg, dict) or msg.get(b"y") != b"q": continue
        q, a, t = msg.get(b"q"), msg.get(b"a") or {}, msg.get(b"t", b"")
        nid = a.get(b"id")
        if not isinstance(nid, bytes) or len(nid) != 20: continue
        new = addr not in nodes
        nodes[addr] = (nid, time.time())
        if new: print("dht: node %s:%d joined (%d known)" % (addr[0], addr[1], len(nodes)), flush=True)
        r = {b"id": me}
        if q == b"ping":
            pass
        elif q == b"find_node":
            r[b"nodes"] = closest(a.get(b"target", b"\0" * 20), addr)
        elif q == b"get_peers":
            h = a.get(b"info_hash", b"")
            r[b"token"] = token(addr[0])
            found = [compact(*p) for p in peers.get(h, {}) if p != addr]
            if found: r[b"values"] = found
            r[b"nodes"] = closest(h, addr)
            print("dht: get_peers on %s from %s:%d: %d peer(s)" % (topic(h), addr[0], addr[1], len(found)), flush=True)
        elif q == b"announce_peer":
            h = a.get(b"info_hash", b"")
            if a.get(b"token") != token(addr[0]):
                s.sendto(bencode({b"t": t, b"y": b"e", b"e": [203, b"bad token"]}), addr); continue
            p = addr[1] if a.get(b"implied_port") == 1 else int(a.get(b"port", 0))
            peers.setdefault(h, {})[(addr[0], p)] = time.time()
            print("dht: announce on %s: %s:%d" % (topic(h), addr[0], p), flush=True)
        else:
            s.sendto(bencode({b"t": t, b"y": b"e", b"e": [204, b"method unknown"]}), addr); continue
        s.sendto(bencode({b"t": t, b"y": b"r", b"r": r}), addr)

if __name__ == "__main__":
    main()
