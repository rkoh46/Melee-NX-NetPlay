#!/usr/bin/env python3
"""Verdict of test-online-linux.sh MU_STADIUM=1: does this side's netplay keep its rhythm through Pokemon Stadium's
transformations, and does the netplay thread's timing (patches/net-0016) tell a stall on this side from the network's?

usage: stadium_stall_check.py RUN_DIR [--stall-frame N --stall-ms MS]
RUN_DIR is build/native-engine-gfx/run-online: {a,b}/console.log, {a,b}/Replays/*.slp and *.trace, {a,b}/user/network.csv.
Side b's netplay thread was held MS ms at online frame N (MELEE_NET_TEST_THREAD_STALL). Checks, each side:
  - the replay is a Pokemon Stadium game (stage 3) with at least two transformations (Slippi's stadium transformation
    events, 0x41), and the game report came before the match ended;
  - side b logged its late input with the test stall named as the step that held it, and side a logged the silence from
    the opponent with its own thread free (so the delay is put on the network or the other side);
  - no other late input (20 ms or more from the game's queue to the socket) and no other silence of 100 ms or more
    after the match's first 300 frames (first-time loads pause a software-rendered test game there);
  - no tick waiting for the opponent's inputs from 30 frames before to 150 frames after each transformation event;
  - network.csv has this build's header and the match's rows, written after the game report (or after the last online
    frame, when the run stops in the match), never during the match.
Prints a table per transformation event and PASS or FAIL. Online frame = replay frame + 124 (frame -123 is the first)."""
# SPDX-License-Identifier: GPL-3.0-or-later
import csv, glob, os, re, struct, sys

run = sys.argv[1]
stall_frame = int(sys.argv[sys.argv.index('--stall-frame') + 1]) if '--stall-frame' in sys.argv else 1500
stall_ms = int(sys.argv[sys.argv.index('--stall-ms') + 1]) if '--stall-ms' in sys.argv else 300
EVENT = {0: 'done', 1: 'rolled', 2: 'starts', 3: 'on the screen', 4: 'old form sinks', 5: 'new form rises', 6: 'finishes'}
FORM = {3: 'Fire', 4: 'Grass', 5: 'Normal', 6: 'Rock', 9: 'Water'}
failures = []

def fail(text):
    failures.append(text); print('FAIL: ' + text)

def slp_events(path):
    """(stage, [(replay frame, event, form)]) from a .slp's raw event stream."""
    data = open(path, 'rb').read()
    at = data.find(b'raw[$U#l')
    if at < 0: return None, []
    size = struct.unpack('>I', data[at + 8:at + 12])[0]; raw = data[at + 12:at + 12 + size]
    if not raw or raw[0] != 0x35: return None, []
    sizes = {raw[k]: struct.unpack('>H', raw[k + 1:k + 3])[0] for k in range(2, 1 + raw[1], 3)}
    p, stage, events = 1 + raw[1], None, []
    while p < len(raw) and raw[p] in sizes:
        code, body = raw[p], raw[p + 1:p + 1 + sizes[raw[p]]]
        if code == 0x36: stage = struct.unpack('>H', body[0x12:0x14])[0]
        elif code == 0x41: events.append((struct.unpack('>i', body[0:4])[0], *struct.unpack('>HH', body[4:8])))
        p += 1 + sizes[code]
    return stage, events

sides = {}
for side in ('a', 'b'):
    d = os.path.join(run, side)
    log = open(os.path.join(d, 'console.log'), errors='replace').read().splitlines() if os.path.exists(os.path.join(d, 'console.log')) else []
    slps = sorted(glob.glob(os.path.join(d, 'Replays', '*.slp')))
    traces = sorted(glob.glob(os.path.join(d, 'Replays', '*.trace')))
    s = {'log': log, 'slp': slps[-1] if slps else None, 'trace': traces[-1] if traces else None}
    s['stage'], s['events'] = slp_events(s['slp']) if s['slp'] else (None, [])
    s['rows'] = list(csv.DictReader(open(traces[-1]))) if traces else []
    net = os.path.join(d, 'user', 'network.csv')
    s['net_header'] = open(net).readline().strip() if os.path.exists(net) else ''
    s['net'] = list(csv.DictReader(open(net))) if os.path.exists(net) else []
    sides[side] = s

def lines(side, pattern):
    return [(i, l) for i, l in enumerate(sides[side]['log']) if re.search(pattern, l)]

late_re = r'netplay thread: our input left ([0-9.]+) ms .*online frame (-?\d+)\); longest step meanwhile: (.*) ([0-9.]+) ms'
quiet_re = r'netplay thread: nothing from the opponent for ([0-9.]+) ms \(online frames (-?\d+) to (-?\d+)\); this side.s thread (.*)'
near_stall = lambda f: stall_frame - 10 <= f <= stall_frame + 60
START = 300   # the match's first seconds: each side's first-time loads (a software-rendered test) pause its game; not judged

for side, s in sides.items():
    print(f'== side {side}: replay {os.path.basename(s["slp"] or "none")}, session trace {os.path.basename(s["trace"] or "none")}, '
          f'{len(s["net"])} network.csv rows')
    if s['stage'] != 3: fail(f'{side}: the replay is not a Pokemon Stadium game (stage {s["stage"]})')
    starts = [e for e in s['events'] if e[1] == 2]
    if len(starts) < 2: fail(f'{side}: {len(starts)} transformations started in the replay (wanted 2 or more: play longer)')
    if not s['rows']: fail(f'{side}: no session trace (settings: session_trace 2)')
    for i, l in lines(side, r'netplay thread: \d+ sends'): print('  ' + l.split('slippi: ', 1)[-1])
    late = [(float(m.group(1)), int(m.group(2)), m.group(3)) for _, l in lines(side, late_re) for m in [re.search(late_re, l)]]
    quiet = [(float(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)) for _, l in lines(side, quiet_re) for m in [re.search(quiet_re, l)]]
    for ms, f, step in late: print(f'  late input: {ms:.1f} ms at online frame {f}, longest step meanwhile: {step}')
    for ms, f0, f1, what in quiet: print(f'  silence from the opponent: {ms:.0f} ms, online frames {f0}..{f1}, this side\'s thread {what}')
    if side == 'b':
        if not any(ms >= stall_ms * 0.8 and near_stall(f) and step.startswith('test stall') for ms, f, step in late):
            fail(f'b: no late input of about {stall_ms} ms naming the test stall near online frame {stall_frame}')
    else:
        if not any(ms >= stall_ms * 0.6 and near_stall(f1) and what.startswith('was free (the network') for ms, f0, f1, what in quiet):
            fail(f'a: no silence of about {stall_ms} ms from the stalled opponent with this side\'s thread free near online frame {stall_frame}')
    others = [x for x in late if not near_stall(x[1]) and x[1] >= START] + [x for x in quiet if not near_stall(x[2]) and x[1] >= START]
    if others: fail(f'{side}: {len(others)} late inputs or silences away from the test stall: {others[:4]}')
    # network.csv: this build's columns, the match's rows, written once the game was reported.
    if 'thread_step_max' not in s['net_header']: fail(f'{side}: network.csv is missing or has an older header')
    samples = [r for r in s['net'] if r['event'] == 'sample']
    if len(samples) < 60: fail(f'{side}: network.csv has {len(samples)} one-second rows (wanted the whole match)')
    # Written once the game is reported, or (a run that stops in the middle of the match) at the end: after the last
    # online frame either way, never while the match ran.
    ended = lines(side, r'slippi: game report:') or lines(side, r'slippi: online frame \d+ wall')[-1:]
    written = lines(side, r'slippi: network diagnostics: \d+ rows written to')
    if not ended or not written or written[0][0] < ended[0][0]:
        fail(f'{side}: network.csv rows were written before the match was over ({len(written)} writes)')
    else: print('  ' + written[0][1].split('slippi: ', 1)[-1])
    s['late'], s['quiet'] = late, quiet

# Per transformation event: waits (session trace), worst queue-to-socket time and longest held-up step (network.csv rows).
print('== transformation events (replay frame / online frame): ticks waiting for the opponent, worst input queue-to-socket ms, '
      'longest held-up netplay step ms, worst ping ms; side a | side b')
events = sides['a']['events'] or sides['b']['events']
for frame, event, form in events:
    online = frame + 124
    cells = []
    for side, s in sides.items():
        rows = [r for r in s['rows'] if online - 30 <= int(r['frame']) <= online + 150]
        waits = sum(1 for r in rows if int(r['wait_frames']) > 0)
        net = [r for r in s['net'] if r['event'] == 'sample' and online - 30 <= int(r['online_frame']) <= online + 210]
        send = max((int(r['send_delay_max_us']) for r in net), default=0) / 1000
        step = max((int(r['thread_step_max_us']) for r in net), default=0) / 1000
        ping = max((int(r['ping_ms']) for r in rows), default=0)
        cells.append(f'{waits:3d} waits, send {send:5.1f}, step {step:5.1f}, ping {ping:4d}')
        if waits and not (online - 30 <= stall_frame + 60 and stall_frame - 10 <= online + 150):
            fail(f'{side}: {waits} ticks waited for the opponent around the {FORM.get(form, form)} event "{EVENT.get(event, event)}" at online frame {online}')
    print(f'  {frame:6d} / {online:6d}  {FORM.get(form, str(form)):6s} {EVENT.get(event, str(event)):15s} {cells[0]} | {cells[1]}')

print('PASS' if not failures else f'FAIL ({len(failures)} problems)')
sys.exit(0 if not failures else 1)
