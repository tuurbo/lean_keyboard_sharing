# Lean Keyboard Sharing

A small keyboard-only switcher for two computers on the same local network.

Press one hotkey to send the Windows keyboard to the other computer. Press it
again to return the keyboard to the local computer. The mouse always stays on
its own computer: the sender does not capture or block it, and the receiver
does not accept or inject mouse input.

## Features

- Keyboard-only sharing
- Press-to-record global toggle hotkey (`Alt+1` by default)
- Direct UDP input with low latency
- Toggle-time receiver connection feedback
- Green and gray keyboard-key tray status icons
- Optional clipboard text and image sync, off by default
- Optional per-user Windows startup
- Single sender and receiver instance per Windows session
- Windows sender and receiver
- Optional Linux sender
- No service, driver, administrator access, or installation

## How it works

While sharing is off, the sender waits for the global hotkey. While sharing is
on, it installs one low-level keyboard hook, forwards key presses and releases,
and blocks those keys on the sender. It never installs a mouse hook, registers
Raw Input for a mouse, confines the cursor, or sends mouse packets.

The receiver accepts only the keyboard protocol and uses Windows `SendInput`
with `INPUT_KEYBOARD`. Packets from the original keyboard-and-mouse v1.2
release are not compatible and are ignored.

The sender and receiver show a keyboard-key tray icon. It is green on the
computer that currently has the keyboard and gray on the other computer.

The receiver acknowledges a status probe when the keyboard toggles. The sender
then shows **Receiver reached** or **No response from receiver**. This check does
not add polling or network traffic while the sender is idle.

Clipboard sync is separate from input and is off by default. If enabled in the
sender settings, the sender pushes its clipboard when sharing turns on and
pulls the receiver clipboard when sharing turns off. Only one clipboard
transfer can run at a time. Transfers accept validated UTF-16 text up to 8 MiB
or validated DIB images up to 64 MiB.

## Build on Windows

Microsoft C++ Build Tools:

```batch
build_msvc.bat
```

MinGW with `gcc` and `windres` on `PATH`:

```batch
build.bat
```

Both commands create `dist\sender.exe` and `dist\receiver.exe`.

## Automated Windows builds

GitHub Actions builds both Windows programs on every push and pull request. It
uploads `sender.exe`, `receiver.exe`, and `SHA256SUMS.txt` as one workflow
artifact.

Push a tag whose name starts with `v` to publish the same files in a GitHub
release:

```bash
git tag v1.3.0
git push origin v1.3.0
```

## Use on Windows

On the computer that will receive the keyboard:

```batch
dist\receiver.exe
```

The receiver runs in the notification area. Right-click its tray icon to see
the current keyboard owner, enable **Start with Windows**, or exit. It does not
require administrator access.

On the computer with the physical keyboard:

```batch
dist\sender.exe
```

Enter the receiver computer's IPv4 address or host name in the settings window
and press the wanted keyboard shortcut in the hotkey field. The settings also
include clipboard sharing and **Start with Windows** checkboxes. Select **Save**.
Double-click the tray icon to open these settings again. Right-click it to
toggle the keyboard, open settings, or exit.

Press `Alt+1` to toggle the keyboard between the two computers. An address on
the command line is still accepted as a temporary override:

```batch
dist\sender.exe 192.168.0.231
```

The settings window creates `sender.cfg` beside `sender.exe`:

```ini
RECEIVER=192.168.0.231
HOTKEY=ALT+1
CLIPBOARD=0
```

Set `CLIPBOARD=1` or select **Enable clipboard sharing** in the settings window
to opt in.

The startup options use the current user's Windows startup registry entry. They
do not require administrator access. Both programs also prevent a second copy
of the same program from running in the current Windows session.

## Build the Linux sender

The Linux sender reads keyboard devices through `evdev`. It does not open or
grab mouse devices.

```bash
./build_linux.sh
./dist/sender_linux 192.168.0.231
```

The user must have permission to read `/dev/input/event*`. The script prints
the required group setup command when the permission is missing.

## Network

- UDP 7777: keyboard input
- TCP 7778: optional clipboard sync at toggle boundaries

Allow these inbound ports for the receiver on the intended local network.
The protocol is not encrypted or authenticated.

## License

This project uses the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0/).
You may use, modify, and share it for personal or educational, non-commercial
purposes. Commercial use requires permission from the original author.
