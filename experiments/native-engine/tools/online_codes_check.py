#!/usr/bin/env python3
"""Checks that the native game plays online with the gameplay codes Slippi Dolphin 3.7.0 runs, and that a replay's code list
maps onto the native game's replay codes the way melee-unlocked maps it. Reads files only; runs nothing.

1. Slippi 3.7.0's Required code sections (port/slippi_sys/GameSettings/GALE01r2.ini: General Codes, Slippi Recording,
   Slippi Online). Each code's injection address is either a gameplay code the native game implements (the replay code
   table of frontend/src/features/replays.cpp), or listed by Slippi's own injection lists as not affecting gameplay
   (the fetched tree's port/slippi_sys_playback/Slippi/InjectionLists), or neither: a gap, a desync risk.
2. The replay codes those sections amount to must be exactly what the game image forces on online: the General Codes'
   (UCF 0.84, neutral spawns, freeze glitch fix: always on) plus mu_online_codes() in the fetched tree's
   sourceport/game/shim/mu_online.c. DynamicsFix (C209E090) is checked to be in the Online section and in the game image
   (mu_slippi_dynamics_fix, run whenever the General Codes are, which an online match forces).
3. The replay code table in replays.cpp must equal melee-unlocked's own (port/app/source_host.cpp replay_codes).

usage: online_codes_check.py [REPO]   (default: this repository; the fetched tree is REPO/build/native-engine/mu)
Exit status 0 when everything matches."""
# SPDX-License-Identifier: GPL-3.0-or-later
import glob, json, os, re, sys

repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', '..'))
tree = os.path.join(repo, 'build', 'native-engine', 'mu')
ok = True


def fail(text):
    global ok
    ok = False
    print('MISMATCH: ' + text)


def table(path, start):
    """{address: code name} from a replay code table ({0x800D65EC, MU_REPLAY_CODE_UCF084...} entries after `start`)."""
    text = open(path, encoding='utf-8').read()
    body = text[text.index(start):]
    body = body[body.index('known[] = {'):]
    body = body[:body.index('};')]
    out = {}
    for address, code in re.findall(r'\{\s*0x([0-9A-Fa-f]{8}),\s*(MU_REPLAY_CODE_\w+|kUcfShared|0)\b', body):
        out[int(address, 16)] = 'UCF (shared site)' if code == 'kUcfShared' else ('not gameplay' if code == '0' else code[len('MU_REPLAY_CODE_'):])
    return out


ours = table(os.path.join(repo, 'experiments/native-engine/frontend/src/features/replays.cpp'), 'uint32_t replay_codes(')
theirs = table(os.path.join(tree, 'port/app/source_host.cpp'), 'uint32_t replay_codes(')
print('replay code table: %d entries in replays.cpp, %d in melee-unlocked\'s source_host.cpp' % (len(ours), len(theirs)))
if ours != theirs:
    for a in sorted(set(ours) | set(theirs)):
        if ours.get(a) != theirs.get(a): fail('replay code %08X: ours %s, melee-unlocked %s' % (a, ours.get(a), theirs.get(a)))
else:
    print('  identical (every injection address and the code it means)')

ignored = set()
for f in glob.glob(os.path.join(tree, 'port/slippi_sys_playback/Slippi/InjectionLists/*.json')):
    for d in json.load(open(f))['Details']: ignored.add(int(d['InjectionAddress'], 16))
print('Slippi\'s injection lists: %d addresses that do not affect gameplay' % len(ignored))

sections, current = {}, None
for line in open(os.path.join(repo, 'port/slippi_sys/GameSettings/GALE01r2.ini'), encoding='utf-8'):
    line = line.rstrip('\n')
    if line.startswith('$'): current = line[1:].split('[')[0].strip(); sections[current] = []; continue
    if line.startswith('['): current = None; continue
    m = re.match(r'^([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{8})', line)
    if current and m: sections[current].append((int(m.group(1), 16), int(m.group(2), 16)))

required = set()
for name in ('Required: General Codes', 'Required: Slippi Recording', 'Required: Slippi Online'):
    words, i, gameplay, quiet, gaps = sections.get(name, []), 0, set(), 0, []
    if not words: fail('section %s not found' % name); continue
    while i < len(words):
        a, b = words[i]
        kind, address, n = (a >> 24) & 0xFE, (a & 0x01FFFFFF) | 0x80000000, 1
        if kind in (0xC0, 0xC2): n = 1 + b
        elif kind == 0x08: n = 2
        elif kind == 0x06: n = 1 + (b + 7) // 8
        if address in ours: gameplay.add(ours[address])
        elif address in ignored: quiet += 1
        else: gaps.append('%02X %08X' % (a >> 24, address))
        i += n
    gameplay.discard('not gameplay')
    required |= gameplay
    print('%s: gameplay codes %s; %d codes that do not affect gameplay; %d neither' % (name, ', '.join(sorted(gameplay)), quiet, len(gaps)))
    for g in gaps: fail('%s: code %s is neither a native gameplay code nor listed as not affecting gameplay' % (name, g))

if 'UCF (shared site)' in required: required.discard('UCF (shared site)')   # 0.84's shared sites: UCF084 is there too
general = {'UCF084', 'NEUTRAL_SPAWN', 'FREEZE_GLITCH'}
shim = open(os.path.join(tree, 'sourceport/game/shim/mu_online.c'), encoding='utf-8').read()
body = shim[shim.index('unsigned int mu_online_codes(void)'):]
body = body[:body.index('\n}')]
online = {c[len('MU_RC_'):] for c in re.findall(r'MU_RC_\w+', body)}
image = general | online
print('the game image online: General Codes %s + mu_online_codes() %s' % (', '.join(sorted(general)), ', '.join(sorted(online))))
if required != image:
    for c in sorted(required - image): fail('Slippi 3.7.0 runs %s online; the game image does not force it' % c)
    for c in sorted(image - required): fail('the game image forces %s online; Slippi 3.7.0\'s Required sections do not have it' % c)
else:
    print('  identical: every gameplay code of Slippi 3.7.0\'s Required sections is on in an online match, and nothing else')

online_words = sections.get('Required: Slippi Online', [])
dynamics = any((a & 0x01FFFFFF) | 0x80000000 == 0x8009E090 and (a >> 24) & 0xFE == 0xC2 for a, _ in online_words)
patch = open(os.path.join(tree, 'sourceport/patches/melee-native.patch'), encoding='utf-8', errors='replace').read()
in_image = 'mu_slippi_dynamics_fix(GET_JOBJ(gobj))' in patch and 'if (mu_general_codes())' in patch
general_online = 'return general_codes || mu_online_codes() != 0;' in open(os.path.join(tree, 'sourceport/game/shim/mu_gecko.c'), encoding='utf-8').read()
print('DynamicsFix (C209E090): in Slippi Online %s, in the game image %s, forced online with the General Codes %s' % (
    'yes' if dynamics else 'NO', 'yes' if in_image else 'NO', 'yes' if general_online else 'NO'))
if not (dynamics and in_image and general_online): fail('DynamicsFix')
print('online codes check: %s' % ('all match' if ok else 'MISMATCHES above'))
sys.exit(0 if ok else 1)
