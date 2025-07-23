#!/usr/bin/env python3
"""
Claude Hooks Python Server Example

Demonstrates security policies:
- Blocks dangerous shell commands (rm -rf, fork bombs, disk operations)
- Prevents access to sensitive files (.env, SSH keys, AWS credentials)
- Modifies file paths to redirect /tmp writes to safe location
"""

from flask import Flask, request, jsonify
import re
import logging

app = Flask(__name__)
logging.basicConfig(level=logging.INFO)

# Block destructive or dangerous shell commands
DANGEROUS_PATTERNS = [
    r'rm\s+-rf\s+/',          # Recursive force delete from root
    r'dd\s+if=/dev/zero\s+of=',  # Disk overwrite
    r':(){ :|:& };:',         # Fork bomb
    r'>\s*/dev/sda',          # Direct disk write
    r'mkfs\.',                # Format filesystem
]

# Protect sensitive configuration and credential files
SENSITIVE_FILES = [
    r'\.env$',                # Environment variables
    r'\.ssh/',                # SSH keys directory
    r'\.aws/',                # AWS credentials
    r'\.git/config',          # Git configuration
    r'private_key',           # Generic private keys
    r'id_rsa',                # RSA private keys
]

@app.route('/hook', methods=['POST'])
def handle_hook():
    try:
        data = request.json
        event = data.get('event', {})
        event_data = data.get('data', {})
        
        event_type = event.get('type')
        logging.info(f"Received {event_type} event")
        
        # Route to appropriate handler based on event type
        if event_type == 'PreToolUse':
            return handle_pre_tool_use(event_data)
        elif event_type == 'PostToolUse':
            return handle_post_tool_use(event_data)
        elif event_type == 'UserPromptSubmit':
            return handle_user_prompt(event_data)
        else:
            # Allow unknown events by default
            return jsonify({
                'version': '1.0',
                'decision': 'allow'
            })
            
    except Exception as e:
        logging.error(f"Error processing request: {e}")
        return jsonify({
            'version': '1.0',
            'decision': 'allow',
            'metadata': {'error': str(e)}
        }), 500

def handle_pre_tool_use(data):
    tool_name = data.get('tool_name')
    tool_input = data.get('tool_input', {})
    
    # Security check: Bash commands
    if tool_name == 'Bash':
        command = tool_input.get('command', '')
        
        for pattern in DANGEROUS_PATTERNS:
            if re.search(pattern, command, re.IGNORECASE):
                return jsonify({
                    'version': '1.0',
                    'decision': 'block',
                    'reason': f'Dangerous command pattern detected: {pattern}'
                })
        
        logging.info(f"Bash command allowed: {command}")
    
    # Security check: File operations
    elif tool_name in ['Read', 'Write', 'Edit']:
        file_path = tool_input.get('file_path', '') or tool_input.get('filePath', '')
        
        for pattern in SENSITIVE_FILES:
            if re.search(pattern, file_path):
                return jsonify({
                    'version': '1.0',
                    'decision': 'block',
                    'reason': f'Access to sensitive file blocked: {file_path}'
                })
        
        # Path sandboxing: Redirect /tmp to safe location
        if file_path.startswith('/tmp/'):
            return jsonify({
                'version': '1.0',
                'decision': 'modify',
                'modified_data': {
                    'tool_input': {
                        **tool_input,
                        'file_path': file_path.replace('/tmp/', '/safe/tmp/')
                    }
                }
            })
    
    return jsonify({
        'version': '1.0',
        'decision': 'allow'
    })

def handle_post_tool_use(data):
    tool_name = data.get('tool_name')
    tool_response = data.get('tool_response', {})
    
    # Example: Check for errors in tool responses
    if tool_response.get('error'):
        logging.warning(f"Tool {tool_name} failed: {tool_response['error']}")
    
    return jsonify({
        'version': '1.0',
        'decision': 'allow'
    })

def handle_user_prompt(data):
    prompt = data.get('prompt', '')
    
    # Example: Block prompts containing secrets
    secret_patterns = [
        r'password\s*[:=]\s*\S+',
        r'api[_-]?key\s*[:=]\s*\S+',
        r'secret\s*[:=]\s*\S+',
    ]
    
    for pattern in secret_patterns:
        if re.search(pattern, prompt, re.IGNORECASE):
            return jsonify({
                'version': '1.0',
                'decision': 'block',
                'reason': 'Prompt contains potential secrets. Please remove sensitive information.'
            })
    
    return jsonify({
        'version': '1.0',
        'decision': 'allow'
    })

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8080, debug=True)