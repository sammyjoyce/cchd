# Claude Code Hooks Dispatcher

[![CI](https://github.com/sammyjoyce/cchd/actions/workflows/ci.yaml/badge.svg)](https://github.com/sammyjoyce/cchd/actions/workflows/ci.yaml)
[![Release](https://img.shields.io/github/v/release/sammyjoyce/cchd)](https://github.com/sammyjoyce/cchd/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Zig Version](https://img.shields.io/badge/Zig-0.14.1-orange.svg)](https://ziglang.org/)

> The fundamental problem of communication is that of reproducing at one point either exactly or approximately a message selected at another point.” — Claude Shannon

A lightweight bridge between Claude Code hooks and custom HTTP servers. Designed for safety, performance, and developer experience—in that order.

## Why

Claude Hooks Dispatcher (cchd) is a simple, fast dispatcher that transforms Claude events into a clean protocol, queries your server, and enforces decisions with precision.

## Installation

### Quick Install via Script

Scripts handle deps (Zig, brew/apt), download signed releases, verify, install to `/usr/local/bin`, and setup `~/.claude/settings.json`.

#### macOS

```bash
curl -sSL https://raw.githubusercontent.com/sammyjoyce/cchd/main/install-macos.sh | bash
```

Clone and run:

```bash
git clone https://github.com/sammyjoyce/cchd.git
cd cchd
./install-macos.sh
```

#### Linux

```bash
curl -sSL https://raw.githubusercontent.com/sammyjoyce/cchd/main/install-linux.sh | bash
```

Clone and run:

```bash
git clone https://github.com/sammyjoyce/cchd.git
cd cchd
./install-linux.sh
```

### Manual Build from Source

- Zig 0.14.1+ (install via brew or https://ziglang.org/download/).
- C compiler for yyjson.
- libcurl dev headers.

```bash
zig build -Doptimize=ReleaseSafe
sudo cp zig-out/bin/cchd /usr/local/bin/
```

### Verify

Assert it's in PATH:

```bash
which cchd
```

Test flow:

```bash
echo '{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls"}}' | cchd
```

## How It Works

1. Claude emits hook event to stdin.
2. cchd reads (bounded buffer), parses (yyjson), transforms to protocol.
3. Sends to your HTTP server (retries with backoff).
4. Server decides: allow (200, {"decision":"allow"}), block (200, {"decision":"block"}), modify (200, {"decision":"modify", "modified_data":{...}}).
5. cchd enforces: exit 0/1, outputs original or modified.

Control flow stays yours: batch if needed in your server.

## Configuration

Scripts create `~/.claude/settings.json` with defaults. Edit for your servers:

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8080/hook"
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8080/hook"
          }
        ]
      }
    ],
    "UserPromptSubmit": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8080/hook"
          }
        ]
      }
    ]
  }
}
```

Per-hook options:

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8080/hook"
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8081/hook --fail-closed"
          }
        ]
      }
    ]
  }
}
```

Flags:

- `--server URL`: Endpoint (default: http://localhost:8080/hook).
- `--timeout MS`: Milliseconds (default: 5000).
- `--fail-closed`: Block if server down (default: fail-open).

Env: `HOOK_SERVER_URL` overrides default.

## Protocol

Detailed in [src/PROTOCOL.md](src/PROTOCOL.md): Versioned JSON, event metadata, data. Why? Compatibility, extensibility.

## Example Servers

In `examples/`: Battle-tested, with safety features.

### Python (Flask)

```bash
cd examples
pip install -r requirements.txt
python3 python_server.py
```

- Blocks dangerous cmds, protects files, modifies paths.

### Node.js (Express)

```bash
cd examples
npm install
node node_server.js
```

- Rate limits, audits, security headers.

### Go

```bash
cd examples
go run go_server.go
```

- Stats, SQL injection/path traversal detection.

### Shell

```bash
cd examples
./simple_server.sh
```

- Basic allow/block.

## Testing

Exhaustive:

```bash
zig build test
```

Covers: Builds, fail modes, servers, responses, codes.

## Structure

- `src/`: Core.
  - `cchd.c`: Dispatcher.
  - `PROTOCOL.md`: Spec.
- `examples/`: Servers.
- `build.zig`: Config.
- `test.zig`: Suite.
- `install-*.sh`: Scripts.

## Requirements

Build: Zig 0.14.1+, C compiler, libcurl-dev.

Runtime: libcurl (yyjson static-linked).

## Troubleshooting

- macOS brew missing: https://brew.sh.
- Linux libcurl: `apt install libcurl4-openssl-dev` (Ubuntu), etc.
- Not in PATH: Add `/usr/local/bin`.
- Hooks fail: Check Claude version, JSON syntax, server, manual test.

## License

MIT - Copyright (c) 2025 Sam Joyce
