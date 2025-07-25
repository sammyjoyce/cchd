#!/usr/bin/env -S uv run --script
# /// script
# dependencies = [
#   "aiohttp>=3.9",
# ]
# ///
"""
Quick-start template for a Claude Code hooks server using UV and aiohttp.
Save as quickstart-python.py and run with: uv run quickstart-python.py.
Or make executable: chmod +x quickstart-python.py && ./quickstart-python.py.

This template provides placeholder functions for each hook event type.
Customize the logic in each handler function for your specific needs.
The template uses UV for zero-setup dependency management and aiohttp
for high-performance async HTTP handling.

Events are sent in CloudEvents JSON format (v1.0):
{
  "specversion": "1.0",
  "type": "com.claudecode.hook.PreToolUse",
  "source": "/claude-code/hooks",
  "id": "unique-id",
  "time": "2024-01-15T10:30:00Z",
  "datacontenttype": "application/json",
  "sessionid": "session-123",
  "data": {
    // Complete unmodified stdin input from Claude.
  }
}

For production use with full security features, see examples/python_server.py.
"""

import json
import asyncio
from datetime import datetime
from aiohttp import web

PORT = 8080

# Handler functions for each event type: These async functions process
# different hook events. Customize these functions to implement your specific
# security policies, logging requirements, or data transformations.

async def handle_pre_tool_use(event_data, session_id):
    """Handle PreToolUse events - decide whether to allow, block, or modify tool usage.
    
    This function receives tool execution requests before they run, allowing you to
    inspect inputs and make security decisions. Return appropriate permission values
    to control Claude's behavior.
    """
    tool_name = event_data.get('tool_name', '')
    tool_input = event_data.get('tool_input', {})
    
    print(f"[PreToolUse] Tool: {tool_name}, Session: {session_id}")
    print(f"  Input: {tool_input}")
    
    # Example: Block dangerous commands. This demonstrates basic security filtering
    # for shell commands, but production systems should use more comprehensive checks.
    if tool_name == 'Bash':
        command = tool_input.get('command', '')
        # Add your security logic here: Consider using allowlists, regex patterns,
        # or external validation services for robust security.
        print(f"  Command: {command}")
    
    # Return decision using modern format (v1.0.59+): The response structure
    # supports both legacy and modern formats for backward compatibility.
    return {
        'version': '1.0',
        # Option 1: Modern permission control (preferred)
        # 'hookSpecificOutput': {
        #     'hookEventName': 'PreToolUse',
        #     'permissionDecision': 'allow',  # or 'deny' or 'ask'
        #     'permissionDecisionReason': 'Command is safe'
        # },
        # Option 2: Legacy format
        # 'decision': 'block',
        # 'reason': 'Dangerous command detected',
        'timestamp': datetime.now().isoformat()
    }

async def handle_post_tool_use(event_data, session_id):
    """Handle PostToolUse events - process tool execution results.
    
    This function receives tool outputs after execution, enabling you to log results,
    scan for sensitive data exposure, or trigger follow-up actions based on outputs.
    """
    tool_name = event_data.get('tool_name', '')
    tool_input = event_data.get('tool_input', {})
    tool_response = event_data.get('tool_response', {})
    
    print(f"[PostToolUse] Tool: {tool_name}, Session: {session_id}")
    print(f"  Input: {tool_input}")
    print(f"  Response: {tool_response}")
    
    # Add your post-execution logic here: Common uses include logging outputs,
    # detecting sensitive data leaks, or updating external systems.
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def handle_user_prompt_submit(event_data, session_id):
    """Handle UserPromptSubmit events - validate or modify user prompts.
    
    This function intercepts user prompts before Claude processes them, allowing
    you to detect prompt injection attempts, filter PII, or add context.
    """
    prompt = event_data.get('prompt', '')
    cwd = event_data.get('current_working_directory', '')
    
    print(f"[UserPromptSubmit] Session: {session_id}")
    print(f"  Prompt: {prompt}")
    print(f"  CWD: {cwd}")
    
    # Add your prompt validation logic here: Consider scanning for injection
    # patterns, PII, or policy violations before allowing prompts through.
    
    return {
        'version': '1.0',
        # Option 1: Add additional context (v1.0.59+)
        # 'hookSpecificOutput': {
        #     'hookEventName': 'UserPromptSubmit',
        #     'additionalContext': 'Remember to follow security guidelines'
        # },
        # Option 2: Block the prompt
        # 'decision': 'block',
        # 'reason': 'Prompt contains sensitive information',
        'timestamp': datetime.now().isoformat()
    }

async def handle_notification(event_data, session_id):
    """Handle Notification events - process notifications.
    
    Notifications are informational events that don't require decisions but can
    be logged, forwarded to monitoring systems, or used to trigger workflows.
    """
    message = event_data.get('message', '')
    title = event_data.get('title', '')
    
    print(f"[Notification] Session: {session_id}")
    print(f"  Title: {title}")
    print(f"  Message: {message}")
    
    # Process notification (no decision needed): These events are ideal for
    # audit logging, metrics collection, or triggering external integrations.
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def handle_stop(event_data, session_id):
    """Handle Stop events - perform cleanup when Claude stops.
    
    This function is called when Claude Code is terminating, allowing you to
    save session state, close connections, or notify external systems.
    """
    stop_hook_active = event_data.get('stop_hook_active', False)
    
    print(f"[Stop] Session: {session_id}")
    print(f"  Stop Hook Active: {stop_hook_active}")
    
    # Add cleanup logic here: Consider saving session data, closing database
    # connections, or sending shutdown notifications to external services.
    
    return {
        'version': '1.0',
        # 'decision': 'block',
        # 'reason': 'Cleanup required',
        'timestamp': datetime.now().isoformat()
    }

async def handle_subagent_stop(event_data, session_id):
    """Handle SubagentStop events - cleanup for subagent termination.
    
    Subagents are separate Claude instances spawned for specific tasks. This
    handler allows different cleanup procedures for subagent lifecycles.
    """
    stop_hook_active = event_data.get('stop_hook_active', False)
    
    print(f"[SubagentStop] Session: {session_id}")
    print(f"  Stop Hook Active: {stop_hook_active}")
    
    # Add subagent cleanup logic here: Subagents may have different resource
    # requirements or session data that needs special handling on termination.
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def handle_pre_compact(event_data, session_id):
    """Handle PreCompact events - process before conversation compaction.
    
    This function fires before Claude compresses conversation history to fit within
    context limits, allowing you to preserve important information externally.
    """
    trigger = event_data.get('trigger', '')
    custom_instructions = event_data.get('custom_instructions', '')
    
    print(f"[PreCompact] Session: {session_id}")
    print(f"  Trigger: {trigger}")
    if custom_instructions:
        print(f"  Instructions: {custom_instructions}")
    
    # Process pre-compaction event (no decision needed): Use this to archive
    # conversation history or extract key information before compaction.
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def webhook_handler(request):
    """Main webhook handler that routes to specific event handlers.
    
    This function receives all webhook requests, validates the CloudEvents format,
    and dispatches to appropriate handlers based on event type. It provides
    consistent error handling and response formatting.
    """
    try:
        # Parse request body (CloudEvents format): aiohttp handles JSON parsing
        # asynchronously, which is efficient for concurrent requests.
        body = await request.json()
        
        # Extract CloudEvents attributes: These fields provide event metadata
        # that's consistent across all event types.
        event_type = body.get('type', '')
        session_id = body.get('sessionid', '')  # CloudEvents extension
        
        # The data field contains the complete unmodified stdin input: This ensures
        # hooks receive exactly what Claude Code sent without transformation.
        event_data = body.get('data', {})
        
        # Route to appropriate handler based on CloudEvents type: This dispatcher
        # pattern makes it easy to add new event types as Claude Code evolves.
        if event_type == 'com.claudecode.hook.PreToolUse':
            response = await handle_pre_tool_use(event_data, session_id)
        elif event_type == 'com.claudecode.hook.PostToolUse':
            response = await handle_post_tool_use(event_data, session_id)
        elif event_type == 'com.claudecode.hook.UserPromptSubmit':
            response = await handle_user_prompt_submit(event_data, session_id)
        elif event_type == 'com.claudecode.hook.Notification':
            response = await handle_notification(event_data, session_id)
        elif event_type == 'com.claudecode.hook.Stop':
            response = await handle_stop(event_data, session_id)
        elif event_type == 'com.claudecode.hook.SubagentStop':
            response = await handle_subagent_stop(event_data, session_id)
        elif event_type == 'com.claudecode.hook.PreCompact':
            response = await handle_pre_compact(event_data, session_id)
        else:
            print(f"[Unknown Event] Type: {event_type}, Session: {session_id}")
            response = {
                'version': '1.0',
                'timestamp': datetime.now().isoformat()
            }
        
        return web.json_response(response)
        
    except Exception as e:
        print(f"Error processing webhook: {e}")
        return web.json_response({
            'error': 'Internal server error',
            'timestamp': datetime.now().isoformat()
        }, status=500)

async def catch_all(request):
    """Handle all other routes.
    
    This catch-all handler provides a helpful error message for requests to
    incorrect endpoints, guiding users to the correct webhook URL.
    """
    return web.json_response({
        'error': 'Not found',
        'message': 'Webhook endpoint is at /hook'
    }, status=404)

def create_app():
    """Create and configure the aiohttp application.
    
    This factory function sets up the web application with proper routing,
    ensuring all HTTP methods are handled and unknown routes return helpful errors.
    """
    app = web.Application()
    
    # Add webhook route for all HTTP methods: Using '*' ensures we handle
    # POST, GET, and other methods gracefully with appropriate responses.
    app.router.add_route('*', '/hook', webhook_handler)
    
    # Catch all other routes: This prevents confusing 404 errors by providing
    # clear guidance about the correct webhook endpoint.
    app.router.add_route('*', '/{path:.*}', catch_all)
    
    return app

async def main():
    """Main function to run the server.
    
    This async function initializes the web server, sets up graceful shutdown
    handling, and runs the event loop until interrupted. It binds to all
    interfaces (0.0.0.0) for development convenience.
    """
    app = create_app()
    
    print(f'🚀 Claude Hooks server listening on http://localhost:{PORT}')
    print(f'📮 Send webhooks to: http://localhost:{PORT}/hook')
    print('\nPress Ctrl+C to stop the server')
    
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, '0.0.0.0', PORT)
    
    try:
        await site.start()
        await asyncio.Event().wait()  # Run forever: This pattern allows the
        # server to run indefinitely while remaining responsive to shutdown signals.
    except KeyboardInterrupt:
        print('\n👋 Shutting down server...')
    finally:
        await runner.cleanup()

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass