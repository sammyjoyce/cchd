# Claude Code Hooks Dispatcher

[![CI](https://github.com/sammyjoyce/cchd/actions/workflows/ci.yaml/badge.svg)](https://github.com/sammyjoyce/cchd/actions/workflows/ci.yaml)
[![Release](https://img.shields.io/github/v/release/sammyjoyce/cchd)](https://github.com/sammyjoyce/cchd/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C](https://img.shields.io/badge/C-11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Zig Version](https://img.shields.io/badge/Zig-0.14.1-orange.svg)](https://ziglang.org/)

> The fundamental problem of communication is that of reproducing at one point either exactly or approximately a message selected at another point.” — Claude Shannon

A lightweight bridge between Claude Code hooks and custom HTTP servers. Designed for safety, performance, and developer experience—in that order.

## Why

Claude Hooks Dispatcher (cchd) is a simple, fast dispatcher that transforms Claude events into a clean protocol, queries your server, and enforces decisions with precision.

## Installation

### Quick Install (Recommended)

One-liner that auto-detects your OS and installs the latest release:

```bash
curl -sSL https://raw.githubusercontent.com/sammyjoyce/cchd/main/install | bash
```

This will:
- Download the latest signed release
- Verify signature (if minisign is available)
- Install to `~/.cchd/bin`
- Configure your shell PATH
- Setup `~/.claude/settings.json`

### Manual Build from Source

Requirements:
- Zig 0.14.1+ (install via brew or https://ziglang.org/download/)
- C compiler for yyjson
- libcurl dev headers

```bash
git clone https://github.com/sammyjoyce/cchd.git
cd cchd
zig build -Doptimize=ReleaseSafe
sudo cp zig-out/bin/cchd /usr/local/bin/
```

### Verify Installation

Check it's in PATH:

```bash
cchd --version
```

Test the dispatcher:

```bash
echo '{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls"}}' | cchd
```

Note: This will fail with "Failed to parse input JSON" if no server is running, which is expected.

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
            "command": "ccdh"
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
            "command": "cchd"
          }
        ]
      }
    ],
    "Notification": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "cchd"
          }
        ]
      }
    ],
    "UserPromptSubmit": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "ccdh"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "cchd"
          }
        ]
      }
    ],
    "SubagentStop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "cchd"
          }
        ]
      }
    ],
    "PreCompact": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "ccdh"
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
            "command": "ccdh"
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

## Quick Start Templates

In `templates/`: Minimal templates with placeholder functions for each hook event type.

### Install and Run a Template

Choose your preferred language:

#### Python
```bash
# Copy template to your project
curl -sSL https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/quickstart-python.py -o hook-server.py

# Run with uv (installs dependencies automatically)
uv run hook-server.py

# Or with pip
pip install flask
python hook-server.py
```

#### TypeScript
```bash
# Copy template to your project
curl -sSL https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/quickstart-typescript.ts -o hook-server.ts

# Run with Bun
bun run hook-server.ts
```

#### Go
```bash
# Copy template to your project
curl -sSL https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/quickstart-go.go -o hook-server.go

# Run directly
go run hook-server.go
```

These templates provide:
- Separate handler functions for each event type (PreToolUse, PostToolUse, etc.)
- Type definitions for request/response structures
- Basic logging of event data
- Comments showing where to add your custom logic

## Example Servers

In `examples/`: Battle-tested production examples with full security features.

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
- `install`: Universal install script.

## Requirements

Build: Zig 0.14.1+, C compiler, libcurl-dev.

Runtime: libcurl (yyjson static-linked).

## Troubleshooting

- macOS brew missing: https://brew.sh.
- Linux libcurl: `apt install libcurl4-openssl-dev` (Ubuntu), etc.
- Not in PATH: Add `/usr/local/bin`.
- Hooks fail: Check Claude version, JSON syntax, server, manual test.

## OpenCLI Compliance

This project adheres to the [OpenCLI specification](https://opencli.org/). The CLI interface is documented in `opencli.json`, which can be used by documentation generators, auto-completion tools, and other OpenCLI-compatible utilities.

## License

MIT - Copyright (c) 2025 Sam Joyce
