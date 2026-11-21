#!/usr/bin/env python3
"""Rebind a player's keys in the game's DirectInput key files (dat/pl1key_dikb.dat, dat/pl2key_dikb.dat).

  scripts/setup/set_keys.py game --player 1 --fire LCTRL
  scripts/setup/set_keys.py game --player 1 --show

Each file starts with five little-endian dwords: left, right, forward, backward, fire (DirectInput DIK_ codes).
A .orig backup is written the first time a file is changed. The game's own key setup menu edits the same files.
"""
import argparse, os, shutil, struct

DIK = {
    'ESC': 0x01, 'TAB': 0x0F, 'RETURN': 0x1C, 'ENTER': 0x1C, 'LCTRL': 0x1D, 'LSHIFT': 0x2A, 'RSHIFT': 0x36,
    'LALT': 0x38, 'SPACE': 0x39, 'RCTRL': 0x9D, 'RALT': 0xB8, 'UP': 0xC8, 'LEFT': 0xCB, 'RIGHT': 0xCD, 'DOWN': 0xD0,
    'INSERT': 0xD2, 'DELETE': 0xD3, 'HOME': 0xC7, 'END': 0xCF, 'PGUP': 0xC9, 'PGDN': 0xD1,
    'KP0': 0x52, 'KP1': 0x4F, 'KP2': 0x50, 'KP3': 0x51, 'KP4': 0x4B, 'KP5': 0x4C, 'KP6': 0x4D, 'KP8': 0x48,
}
for i, ch in enumerate('QWERTYUIOP'): DIK[ch] = 0x10 + i
for i, ch in enumerate('ASDFGHJKL'): DIK[ch] = 0x1E + i
for i, ch in enumerate('ZXCVBNM'): DIK[ch] = 0x2C + i
for i, ch in enumerate('1234567890'): DIK[ch] = 0x02 + i
NAMES = {v: k for k, v in reversed(list(DIK.items()))}
SLOTS = ['left', 'right', 'forward', 'backward', 'fire']

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('game_dir')
ap.add_argument('--player', type=int, choices=(1, 2), default=1)
ap.add_argument('--show', action='store_true')
for s in SLOTS: ap.add_argument('--' + s, metavar='KEY', help='key name, e.g. ' + ', '.join(list(DIK)[:6]))
a = ap.parse_args()

path = os.path.join(a.game_dir, 'dat', f'pl{a.player}key_dikb.dat')
data = bytearray(open(path, 'rb').read())
keys = list(struct.unpack_from('<5I', data))
changed = False
for i, s in enumerate(SLOTS):
    v = getattr(a, s)
    if v:
        code = DIK.get(v.upper())
        if code is None: ap.error(f'unknown key {v}')
        keys[i] = code; changed = True
if changed:
    if not os.path.exists(path + '.orig'): shutil.copyfile(path, path + '.orig')
    struct.pack_into('<5I', data, 0, *keys)
    open(path, 'wb').write(data)
print(f'player {a.player}: ' + ', '.join(f'{s}={NAMES.get(k, hex(k))}' for s, k in zip(SLOTS, keys)))
