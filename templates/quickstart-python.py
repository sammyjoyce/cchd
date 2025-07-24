#!/usr/bin/env -S uv run --script
# /// script
# dependencies = [
#   "aiohttp>=3.9",
# ]
# ///
"""
Quick-start template for a Claude Code hooks server using UV and aiohttp
Save as quickstart-python.py and run with: uv run quickstart-python.py
Or make executable: chmod +x quickstart-python.py && ./quickstart-python.py

This template provides placeholder functions for each hook event type.
Customize the logic in each handler function for your specific needs.

For production use with full security features, see examples/python_server.py
"""

import json
import asyncio
from datetime import datetime
from aiohttp import web

PORT = 8080

# Handler functions for each event type
# Customize these functions to implement your specific logic

async def handle_pre_tool_use(event_data):
    """Handle PreToolUse events - decide whether to allow, block, or modify tool usage"""
    tool_name = event_data.get('tool_name', '')
    tool_input = event_data.get('tool_input', {})
    session_id = event_data.get('session_id', '')
    
    print(f"[PreToolUse] Tool: {tool_name}, Session: {session_id}")
    print(f"  Input: {tool_input}")
    
    # Example: Block dangerous commands
    if tool_name == 'Bash':
        command = tool_input.get('command', '')
        # Add your security logic here
        print(f"  Command: {command}")
    
    # Return decision using modern format (v1.0.59+)
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

async def handle_post_tool_use(event_data):
    """Handle PostToolUse events - process tool execution results"""
    tool_name = event_data.get('tool_name', '')
    tool_input = event_data.get('tool_input', {})
    tool_response = event_data.get('tool_response', {})
    session_id = event_data.get('session_id', '')
    
    print(f"[PostToolUse] Tool: {tool_name}, Session: {session_id}")
    print(f"  Input: {tool_input}")
    print(f"  Response: {tool_response}")
    
    # Add your post-execution logic here
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def handle_user_prompt_submit(event_data):
    """Handle UserPromptSubmit events - validate or modify user prompts"""
    prompt = event_data.get('prompt', '')
    session_id = event_data.get('session_id', '')
    cwd = event_data.get('current_working_directory', '')
    
    print(f"[UserPromptSubmit] Session: {session_id}")
    print(f"  Prompt: {prompt}")
    print(f"  CWD: {cwd}")
    
    # Add your prompt validation logic here
    
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

async def handle_notification(event_data):
    """Handle Notification events - process notifications"""
    message = event_data.get('message', '')
    title = event_data.get('title', '')
    session_id = event_data.get('session_id', '')
    
    print(f"[Notification] Session: {session_id}")
    print(f"  Title: {title}")
    print(f"  Message: {message}")
    
    # Process notification (no decision needed)
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def handle_stop(event_data):
    """Handle Stop events - perform cleanup when Claude stops"""
    stop_hook_active = event_data.get('stop_hook_active', False)
    session_id = event_data.get('session_id', '')
    
    print(f"[Stop] Session: {session_id}")
    print(f"  Stop Hook Active: {stop_hook_active}")
    
    # Add cleanup logic here
    
    return {
        'version': '1.0',
        # 'decision': 'block',
        # 'reason': 'Cleanup required',
        'timestamp': datetime.now().isoformat()
    }

async def handle_subagent_stop(event_data):
    """Handle SubagentStop events - cleanup for subagent termination"""
    stop_hook_active = event_data.get('stop_hook_active', False)
    session_id = event_data.get('session_id', '')
    
    print(f"[SubagentStop] Session: {session_id}")
    print(f"  Stop Hook Active: {stop_hook_active}")
    
    # Add subagent cleanup logic here
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def handle_pre_compact(event_data):
    """Handle PreCompact events - process before conversation compaction"""
    trigger = event_data.get('trigger', '')
    custom_instructions = event_data.get('custom_instructions', '')
    session_id = event_data.get('session_id', '')
    
    print(f"[PreCompact] Session: {session_id}")
    print(f"  Trigger: {trigger}")
    if custom_instructions:
        print(f"  Instructions: {custom_instructions}")
    
    # Process pre-compaction event (no decision needed)
    
    return {
        'version': '1.0',
        'timestamp': datetime.now().isoformat()
    }

async def webhook_handler(request):
    """Main webhook handler that routes to specific event handlers"""
    try:
        # Parse request body
        body = await request.json()
        
        # Extract event information
        event = body.get('event', {})
        event_type = event.get('type', '')
        session_id = event.get('session_id', '')
        
        # Merge event metadata with data for easier access
        event_data = body.get('data', {})
        event_data['session_id'] = session_id
        
        # Route to appropriate handler based on event type
        if event_type == 'PreToolUse':
            response = await handle_pre_tool_use(event_data)
        elif event_type == 'PostToolUse':
            response = await handle_post_tool_use(event_data)
        elif event_type == 'UserPromptSubmit':
            response = await handle_user_prompt_submit(event_data)
        elif event_type == 'Notification':
            response = await handle_notification(event_data)
        elif event_type == 'Stop':
            response = await handle_stop(event_data)
        elif event_type == 'SubagentStop':
            response = await handle_subagent_stop(event_data)
        elif event_type == 'PreCompact':
            response = await handle_pre_compact(event_data)
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
    """Handle all other routes"""
    return web.json_response({
        'error': 'Not found',
        'message': 'Webhook endpoint is at /hook'
    }, status=404)

def create_app():
    """Create and configure the aiohttp application"""
    app = web.Application()
    
    # Add webhook route for all HTTP methods
    app.router.add_route('*', '/hook', webhook_handler)
    
    # Catch all other routes
    app.router.add_route('*', '/{path:.*}', catch_all)
    
    return app

async def main():
    """Main function to run the server"""
    app = create_app()
    
    print(f'🚀 Claude Hooks server listening on http://localhost:{PORT}')
    print(f'📮 Send webhooks to: http://localhost:{PORT}/hook')
    print('\nPress Ctrl+C to stop the server')
    
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, '0.0.0.0', PORT)
    
    try:
        await site.start()
        await asyncio.Event().wait()  # Run forever
    except KeyboardInterrupt:
        print('\n👋 Shutting down server...')
    finally:
        await runner.cleanup()

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass