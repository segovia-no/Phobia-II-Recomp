# Playing

- Start `build/phobia2`. First screen: pick 16-bit or 8-bit colour with the arrow keys and Enter (8-bit has the light effects).
- Keys are stored in `game/dat/pl1key_dikb.dat` / `pl2key_dikb.dat` (the game's setup menu edits them too). Rebind from the terminal:
  ```sh
  .venv/bin/python scripts/setup/set_keys.py game --player 1 --fire LCTRL
  .venv/bin/python scripts/setup/set_keys.py game --player 1 --show
  ```
  The original default fire key is Enter. On macOS, Ctrl+arrow keys switch Spaces / open Mission Control by default; if that interferes with firing while turning, turn those shortcuts off in System Settings → Keyboard → Keyboard Shortcuts → Mission Control, or bind fire to another key (e.g. `--fire SPACE`).
- Esc goes back.
- `Alt+Enter` or `Cmd+Ctrl+F`: toggle fullscreen. The window is resizable.
- A gamepad (any SDL-supported controller) appears to the game as joystick 1: left stick / d-pad and A/B/X/Y.
