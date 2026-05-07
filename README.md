# Vibe Bridge

Vibe Bridge is a desktop host application for the Vibe Coding Keyboard. It bridges the physical hardware (keys, knob, LCD) with AI coding tools.

## Features
- **HID Enumeration**: Automatically finds Vibe Keyboard via Usage Page `0xFF60`.
- **Input Parsing**: Decodes 7 custom keys and knob rotation/press.
- **Screen Output**: Supports text output, cursor control, and clearing the screen via HID.
- **ACK Handling**: Reliable communication with flow control (busy/ok/err).
- **Claude Integration**: Formats and sends Claude status to the physical screen.

## Project Structure
- `src/hid/`: HID communication layer (device, reports, screen).
- `src/claude/`: Claude status integration and formatting.
- `docs/protocol.md`: Detailed HID protocol specification.

## Prerequisites
- Rust (Cargo)
- `libusb-1.0-0-dev` and `libudev-dev` (on Linux)

## How to Run

### 0. Run the daemon

For fast updates, keep one daemon process alive. It opens HID once, scans the
current WSL/Linux process namespace for Claude/Codex/shell sessions, and listens
for hook events on `/tmp/vibe-bridge.sock`.

```bash
cargo build
target/debug/vibe-bridge daemon
```

### 1. Scan for devices
```bash
cargo run -- scan
```

### 2. Listen for button/knob events
```bash
cargo run -- listen
```

### 3. Test screen output
```bash
cargo run -- test-screen --text "Hello Vibe!"
```

### 4. Send Claude status
```bash
cargo run -- send-claude
```

`send-claude` scans the current WSL/Linux process namespace for a running
Claude process and sends that process' cwd/git branch to the LCD. Processes in
another WSL distro are not visible from this Linux namespace; run Vibe Bridge in
the same distro/session as Claude for automatic detection. Use `--mock` for the
static demo frame.

Screen writes default to fast fire-and-forget HID output. Use `--reliable` only
when debugging ACK/busy/error handling:

```bash
cargo run -- test-screen --text "Hello Vibe!" --reliable
cargo run -- send-claude --reliable
```

For lower startup latency after building, run the binary directly:

```bash
cargo build
target/debug/vibe-bridge send-claude
```

## Terminal Hooks

The daemon passively detects many running WSL/VSCode terminal processes through
`/proc`, but shell hooks give lower-latency command state updates. Generate a
bash/zsh snippet and source it from terminals that should report activity:

```bash
target/debug/vibe-bridge hook-script > ~/.vibe-bridge-hook.sh
printf '\n[ -f ~/.vibe-bridge-hook.sh ] && . ~/.vibe-bridge-hook.sh\n' >> ~/.bashrc
```

VSCode Remote WSL integrated terminals inherit the same shell startup files, so
this also covers those terminal sessions.

A different WSL distro is a different process namespace, so this daemon cannot
discover it through `/proc`. Start the HID daemon with a TCP event listener and
run a no-HID forwarder inside each other distro:

```bash
target/debug/vibe-bridge daemon --tcp 0.0.0.0:9797
target/debug/vibe-bridge forward-sessions --tcp 127.0.0.1:9797
```

If the other distro cannot reach `127.0.0.1:9797`, use the daemon distro IP
instead. Shell hooks can still be installed for low-latency command state:

```bash
target/debug/vibe-bridge hook-script --tcp 127.0.0.1:9797 > ~/.vibe-bridge-hook.sh
printf '\n[ -f ~/.vibe-bridge-hook.sh ] && . ~/.vibe-bridge-hook.sh\n' >> ~/.bashrc
```

AI details are enriched from local tool state when available:

- Claude Code: reads `~/.claude/projects/**/*.jsonl` transcripts for `model`,
  token usage, estimated cost, latest user prompt, latest AI reply, and context
  progress.
- Codex: reads `~/.codex/log/codex-tui.log` for `model`, token usage, estimated
  cost, and context progress. Reply text is not currently available from a
  stable plaintext Codex log source.
- Gemini: reads `~/.gemini/tmp/*/chats/*.jsonl` for `model`, token usage,
  estimated cost, latest user prompt/reply, and context progress.

Costs marked with `$~` are local estimates. The 5-hour rolling reset/quota
fields still require a tool hook or API source; they are not present in ordinary
process metadata.

Manual AI event test with richer metadata:

```bash
target/debug/vibe-bridge emit --source vscode --tool Codex --state replying \
  --cwd "$PWD" --model gpt-5.4 --cost '$0.01' --quota "4h 12m left" \
  --quota-reset "18:00" --context 42 --reply "drafted firmware patch"
```

Manual event test:

```bash
target/debug/vibe-bridge emit --source vscode --tool Claude --state running \
  --cwd "$PWD" --message "manual status update"
```

Check what passive `/proc` scanning can see:

```bash
target/debug/vibe-bridge sessions
target/debug/vibe-bridge forward-sessions --tcp 127.0.0.1:9797 --once
```

## Specifying Device
If multiple devices are found, specify VID/PID:
```bash
cargo run -- listen --vid 0x1234 --pid 0x5678
```

## Permissions
On Linux, you may need a udev rule to access the HID device without sudo.
Create `/etc/udev/rules.d/99-vibe.rules`:
```
SUBSYSTEM=="usb", ATTR{idVendor}=="YOUR_VID", ATTR{idProduct}=="YOUR_PID", MODE="0666"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="YOUR_VID", ATTRS{idProduct}=="YOUR_PID", MODE="0666"
```
Replace `YOUR_VID` and `YOUR_PID` with actual values from `scan`.
