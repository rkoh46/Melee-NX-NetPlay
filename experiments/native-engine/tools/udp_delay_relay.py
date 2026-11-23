#!/usr/bin/env python3
"""A UDP relay that adds delay and jitter between two local netplay instances (loopback only).

Instance A listens on A_PORT and is told its peer is 127.0.0.1:B_SIDE; instance B listens on B_PORT
and is told its peer is 127.0.0.1:A_SIDE. Whatever arrives on B_SIDE (from A) leaves from A_SIDE to
B after the delay, and the other way round, so each side sees the address it expects.

usage: udp_delay_relay.py A_PORT A_SIDE B_PORT B_SIDE DELAY_MS JITTER_MS [LOSS_PERCENT] [SEED]"""
# SPDX-License-Identifier: GPL-3.0-or-later
import heapq, random, select, socket, sys, time

a_port, a_side, b_port, b_side = (int(v) for v in sys.argv[1:5])
delay, jitter = float(sys.argv[5]) / 1000, float(sys.argv[6]) / 1000
loss = float(sys.argv[7]) / 100 if len(sys.argv) > 7 else 0.0
rng = random.Random(int(sys.argv[8]) if len(sys.argv) > 8 else 1)

def bound(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', port)); s.setblocking(False)
    return s
as_a, as_b = bound(a_side), bound(b_side)      # as_a stands for A (talks to B); as_b stands for B (talks to A)
queue, order, counts = [], 0, [0, 0, 0]
while True:
    timeout = max(0.0, queue[0][0] - time.monotonic()) if queue else 1.0
    ready, _, _ = select.select([as_a, as_b], [], [], timeout)
    now = time.monotonic()
    for s in ready:
        try: data, _ = s.recvfrom(65536)
        except BlockingIOError: continue
        # Arrived on as_b: it came from A and goes to B, sent from as_a. And the reverse.
        out, port = (as_a, b_port) if s is as_b else (as_b, a_port)
        if rng.random() < loss: counts[2] += 1; continue
        order += 1
        heapq.heappush(queue, (now + delay + rng.uniform(0, jitter), order, out, port, data))
    while queue and queue[0][0] <= time.monotonic():
        _, _, out, port, data = heapq.heappop(queue)
        try: out.sendto(data, ('127.0.0.1', port)); counts[0 if out is as_a else 1] += 1
        except OSError: pass
