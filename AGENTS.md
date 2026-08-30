# Agent Guidelines

This repository is a small keyboard-only sharing tool. Keep it keyboard-only.

## Architecture

- The Windows sender uses one low-level keyboard hook while capture is active.
- The Windows receiver accepts `KeyboardPacket` messages and injects only
  `INPUT_KEYBOARD` events.
- The Linux sender opens and exclusively grabs keyboard `evdev` devices only.
- Mouse hooks, Raw Input mouse registration, cursor confinement, mouse device
  access, and `INPUT_MOUSE` are not allowed.
- Clipboard synchronization is separate and runs only when capture toggles.

## Constraints

- The Windows sender must have no polling, timers, background threads, or
  network activity while idle. Only the registered hotkey remains active.
- Keep both Windows programs windowless.
- Do not require administrator access, UIAccess, signing, installation,
  services, or drivers.
- Use only Win32/POSIX APIs and standard libraries.
- Keep `keyboard_protocol.h` shared by every sender, receiver, and debug tool.

## Build

- `build_msvc.bat`: Microsoft C++ Build Tools
- `build.bat`: MinGW
- `build_linux.sh`: Linux sender
- All compiled programs go in `dist/`.
