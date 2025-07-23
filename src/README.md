# Hook Dispatcher Implementation

This directory contains the core hook dispatcher implementation and protocol documentation.

## Files

- `hook_dispatcher.c` - Main implementation using yyjson for high-performance JSON parsing
- `PROTOCOL.md` - Complete protocol specification for server implementations
- `examples/` - Example server implementations in various languages

## Protocol Overview

### Request Format
```json
{
  "version": "1.0",
  "event": {
    "id": "evt_1234567890",
    "type": "PreToolUse",
    "name": "PreToolUse",
    "timestamp": 1234567890123,
    "session_id": "session-123"
  },
  "data": {
    "tool_name": "Bash",
    "tool_input": {
      "command": "ls -la"
    }
  }
}
```

### Response Format
```json
{
  "version": "1.0",
  "decision": "allow"  // or "block" or "modify"
}
```

For complete protocol details, see [PROTOCOL.md](PROTOCOL.md).

## Example Usage

### Testing Directly
```bash
echo '{"session_id":"test","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"echo hello"}}' | ./cchd
```

### With a Python Server
```python
from flask import Flask, request, jsonify

app = Flask(__name__)

@app.route('/hook', methods=['POST'])
def handle_hook():
    event = request.json
    
    # Block dangerous commands
    if event['data']['tool_name'] == 'Bash':
        cmd = event['data']['tool_input']['command']
        if 'rm -rf' in cmd:
            return jsonify({
                "version": "1.0",
                "decision": "block",
                "reason": "Dangerous command detected"
            })
    
    return jsonify({"version": "1.0", "decision": "allow"})

if __name__ == '__main__':
    app.run(port=8080)
```

See the `examples/` directory for more complete server implementations.