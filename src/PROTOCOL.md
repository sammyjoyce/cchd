# Claude Hooks Server Protocol

## Overview

The Claude Hooks Server Protocol defines a simple JSON-based communication protocol between Claude hooks and external servers. Servers can be written in any language and can accept, block, or modify hook events.

## Communication Flow

1. **Hook Event** → **Hook Dispatcher** → **Server** → **Response** → **Claude**

## Protocol Specification

### Request Format (Hook → Server)

All requests are sent as HTTP POST to the server endpoint with JSON body:

```json
{
  "version": "1.0",
  "event": {
    "id": "unique-event-id",
    "type": "PreToolUse|PostToolUse|UserPromptSubmit|Notification|Stop|SubagentStop|PreCompact",
    "name": "PreToolUse",
    "timestamp": 1234567890123,
    "session_id": "session-123",
    "correlation_id": "correlation-456"
  },
  "data": {
    // Event-specific data
  }
}
```

### Event-Specific Data

#### PreToolUse
```json
{
  "tool_name": "Bash|Read|Write|Edit|...",
  "tool_input": {
    // Tool-specific parameters
  }
}
```

#### PostToolUse
```json
{
  "tool_name": "Bash|Read|Write|Edit|...",
  "tool_input": {
    // Tool-specific parameters
  },
  "tool_response": {
    // Tool execution result
  }
}
```

#### UserPromptSubmit
```json
{
  "prompt": "User's prompt text"
}
```

#### Notification
```json
{
  "message": "Notification message"
}
```

#### Stop/SubagentStop
```json
{
  "stop_hook_active": true|false
}
```

#### PreCompact
```json
{
  "trigger": "manual|auto",
  "custom_instructions": "optional instructions"
}
```

### Response Format (Server → Hook)

```json
{
  "version": "1.0",
  "decision": "allow|block|modify",
  "reason": "Optional human-readable reason",
  "modified_data": {
    // Optional: Modified event data (only for decision: "modify")
  },
  "metadata": {
    // Optional: Server-specific metadata
  }
}
```

### Decision Types

- **allow**: Event proceeds normally
- **block**: Event is blocked with reason sent to Claude
- **modify**: Event proceeds with modified data

### HTTP Status Codes

- **200 OK**: Request processed successfully
- **400 Bad Request**: Invalid request format
- **500 Internal Server Error**: Server error
- **503 Service Unavailable**: Server temporarily unavailable

### Timeout

Default timeout: 5 seconds (configurable)

## Example Implementations

### Python Server Example

```python
from flask import Flask, request, jsonify

app = Flask(__name__)

@app.route('/hook', methods=['POST'])
def handle_hook():
    data = request.json
    
    # Process the event
    event_type = data['event']['type']
    
    if event_type == 'PreToolUse':
        tool_name = data['data']['tool_name']
        
        # Example: Block dangerous commands
        if tool_name == 'Bash':
            command = data['data']['tool_input'].get('command', '')
            if 'rm -rf /' in command:
                return jsonify({
                    'version': '1.0',
                    'decision': 'block',
                    'reason': 'Dangerous command detected'
                })
    
    # Default: allow
    return jsonify({
        'version': '1.0',
        'decision': 'allow'
    })

if __name__ == '__main__':
    app.run(port=8080)
```

### Node.js Server Example

```javascript
const express = require('express');
const app = express();

app.use(express.json());

app.post('/hook', (req, res) => {
    const { event, data } = req.body;
    
    // Process the event
    if (event.type === 'PreToolUse' && data.tool_name === 'Write') {
        // Example: Modify file paths
        if (data.tool_input.file_path.startsWith('/tmp/')) {
            return res.json({
                version: '1.0',
                decision: 'modify',
                modified_data: {
                    tool_input: {
                        ...data.tool_input,
                        file_path: data.tool_input.file_path.replace('/tmp/', '/safe/tmp/')
                    }
                }
            });
        }
    }
    
    // Default: allow
    res.json({
        version: '1.0',
        decision: 'allow'
    });
});

app.listen(8080);
```

## Configuration

The hook dispatcher can be configured with:

```json
{
  "server_url": "http://localhost:8080/hook",
  "timeout_ms": 5000,
  "retry_count": 3,
  "retry_delay_ms": 1000,
  "fail_open": true  // If true, allow on server failure
}
```