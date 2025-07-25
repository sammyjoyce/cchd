# Claude Code Hooks Dispatcher

[![CI](https://github.com/sammyjoyce/cchd/actions/workflows/ci.yaml/badge.svg)](https://github.com/sammyjoyce/cchd/actions/workflows/ci.yaml)
[![Release](https://img.shields.io/github/v/release/sammyjoyce/cchd)](https://github.com/sammyjoyce/cchd/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23)
[![Zig Version](https://img.shields.io/badge/Zig-master-orange.svg)](https://ziglang.org/)

> The fundamental problem of communication is that of reproducing at one point either exactly or approximately a message selected at another point." — Claude Shannon

A lightweight bridge between Claude Code hooks and local or remote servers.

## Why

Claude Hooks Dispatcher (cchd) is a simple, fast dispatcher that transforms Claude events into a clean protocol, queries your server, and enforces decisions with precision. We built this because Claude Code's hook system needs a reliable bridge to custom security policies, and existing solutions were either too complex or too slow for production use.

## Installation

### Quick Install (Recommended)

One-liner that auto-detects your OS and installs the latest release:

```bash
curl -sSL https://raw.githubusercontent.com/sammyjoyce/cchd/main/install | bash
```

This will:

- Download the latest signed release.
- Verify signature (if minisign is available) to ensure authenticity.
- Install to `~/.cchd/bin` to avoid requiring sudo privileges.
- Configure your shell PATH for immediate availability.
- Setup `~/.claude/settings.json` with sensible defaults.

### Manual Build from Source

Requirements:

- Zig master branch (install via zvm: https://github.com/tristanisham/zvm). We use master for the latest C23 support.
- libcurl dev headers for HTTP communication.
- Uses arocc (https://github.com/Vexu/arocc) for C compilation because it provides better C23 compatibility than system compilers.

```bash
git clone https://github.com/sammyjoyce/cchd.git
cd cchd
zig build -Doptimize=ReleaseSafe
sudo cp zig-out/bin/cchd /usr/local/bin/
```

### Verify Installation

Check it's in PATH to ensure the installation succeeded:

```bash
cchd --version
```

Test the dispatcher to verify it can process hook events:

```bash
echo '{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls"}}' | cchd
```

Note: This will fail with a connection error if no server is running, which is expected. This error confirms the dispatcher is working but has no server to connect to.

### Initialize a Template

Create a hook server template in your preferred language:

```bash
# Interactive selection
cchd init

# Or specify language directly
cchd init python
cchd init typescript
cchd init go
```

## How It Works

1. Claude emits hook events to stdin.
2. cchd reads the event using bounded buffers (preventing memory exhaustion), parses with yyjson (for speed), and transforms to the CloudEvent schema.
3. Sends the transformed event to your HTTP server with automatic retries and exponential backoff to handle transient failures.
4. Your server responds with a decision: allow (200, {"decision":"allow"}), block (200, {"decision":"block"}), or modify (200, {"decision":"modify", "modified_data":{...}}). This gives you complete control over Claude's behavior.
5. cchd enforces the decision by exiting with appropriate codes (0 for allow, 1 for block) and outputs either the original or modified data.

Control flow stays with your server - you can batch decisions, check against policy engines, or integrate with existing security infrastructure.

## Configuration

cchd can be configured through multiple methods, listed in order of priority (highest to lowest). This hierarchy allows you to override settings for specific use cases while maintaining global defaults:

1. **Command-line flags** (highest priority)
2. **Environment variables**
3. **Configuration file** (~/.config/cchd/config.json)
4. **Default values**

### Configuration File

Create a configuration file at one of these locations:

- `$CCHD_CONFIG_PATH` (if set)
- `~/.config/cchd/config.json`
- `/etc/cchd/config.json`

Example `config.json` with common settings:

```json
{
  "server_url": "https://my-server.com/hook",
  "timeout_ms": 10000,
  "fail_open": false,
  "debug": false
}
```

### Claude Settings

The installer creates `~/.claude/settings.json` with defaults. Edit this file to configure which hooks are active and which server handles each event type:

```json
{
  "hooks": {
    "PreToolUse": [
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
            "command": "cchd"
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
            "command": "cchd"
          }
        ]
      }
    ]
  }
}
```

Per-hook options allow different servers or settings for each event type. This flexibility lets you route security-critical events to one server while sending informational events elsewhere:

```json
{
  "hooks": {
    "PreToolUse": [
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
    "PostToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8081/hook"
          }
        ]
      }
    ]
  }
}
```

### Command-line Options

- `--server URL`: HTTP server endpoint (default: http://localhost:8080/hook). Use HTTPS in production.
- `--timeout MS`: Request timeout in milliseconds (default: 5000). Increase for slower servers.
- `--fail-open`: Allow operations if server is unavailable (default behavior is fail-closed for security).
- `--api-key KEY`: Set API key for server authentication.
- `-d, --debug`: Enable debug output to troubleshoot connection issues.
- `-q, --quiet`: Suppress non-essential output for cleaner logs.
- `--json`: Output in JSON format for programmatic consumption.
- `--plain`: Output in plain text format without formatting.
- `--no-color`: Disable colored output (also respects NO_COLOR environment variable).
- `--no-input`: Exit immediately without reading input (useful for testing).
- `--insecure`: Disable SSL certificate verification (use with caution in development only).
- `-h, --help`: Show detailed help with examples.
- `--version`: Show version information for bug reports.

### Environment Variables

- `HOOK_SERVER_URL`: Default server URL (overridden by --server flag). Useful for containerized deployments.
- `HOOK_API_KEY`: API key for authentication.
- `CCHD_CONFIG_PATH`: Path to configuration file when not using default locations.
- `NO_COLOR`: Disable colored output when set. Follows the NO_COLOR standard for accessibility.

## Quick Start Templates

The easiest way to get started is using the `init` command, which creates a working hook server template in your preferred language. These templates include placeholder functions for each hook event type, helping you get started quickly without wrestling with protocol details or boilerplate code.

### Create a Template

Use the init command to create a hook server:

```bash
# Interactive selection - choose from available languages
cchd init

# Or specify your language directly
cchd init python
cchd init typescript
cchd init go
```

This will:
- Download the appropriate template for your language
- Save it to your current directory as `hook-server.<ext>`
- Display instructions for running the server

### Run Your Server

After initialization, run your server based on the language:

#### Python

```bash
# Run with uv (installs dependencies automatically)
uv run hook-server.py

# Or with pip
pip install flask
python hook-server.py
```

#### TypeScript

```bash
# Run with Bun
bun run hook-server.ts
```

#### Go

```bash
# Run directly
go run hook-server.go
```

### What Templates Provide

- Separate handler functions for each event type (PreToolUse, PostToolUse, etc.) to keep your code organized.
- Type definitions for request/response structures to prevent common errors.
- Basic logging of event data so you can see what Claude is doing.
- Clear comments showing exactly where to add your custom logic—no guesswork required.

## Testing

Run the comprehensive test suite:

```bash
zig build test
```

The test suite covers: build verification, failure modes, template servers, response handling, and exit codes. This exhaustive testing ensures reliability across different configurations and error scenarios.

## Structure

- `src/`: Core implementation.
  - `cchd.c`: Main dispatcher handling all event processing.
- `templates/`: Quick start templates for Python, TypeScript, and Go.
- `build.zig`: Build configuration using Zig's build system.
- `test.zig`: Comprehensive test suite with real server integration.
- `install`: Universal install script that works across platforms.

## Requirements

Build requirements: Zig (master branch recommended for best C23 support), C compiler, libcurl-dev (for HTTP).

Runtime requirements: Only libcurl is needed at runtime. yyjson is statically linked to avoid dependency issues.

## Troubleshooting

- macOS brew missing: Install from https://brew.sh to get required dependencies.
- Linux libcurl missing: Install with `apt install libcurl4-openssl-dev` (Ubuntu/Debian) or equivalent for your distribution.
- Command not found: Add `/usr/local/bin` to your PATH or use the installer which handles this automatically.
- Hooks not triggering: Check Claude Code version supports hooks, verify JSON syntax in settings.json, ensure server is running, and test manually with the echo command above.

## OpenCLI Compliance

This project adheres to the [OpenCLI specification](https://opencli.org/). The CLI interface is documented in `opencli.json`, which can be used by documentation generators, auto-completion tools, and other OpenCLI-compatible utilities. This standardization ensures consistent behavior and enables better tooling integration.

## License

MIT - Copyright (c) 2025 Sam Joyce
