#!/usr/bin/env bun
// Quick-start template for a Claude Code hooks server using Bun
// Save as quickstart-typescript.ts and run with: bun run quickstart-typescript.ts
// 
// This template provides placeholder functions for each hook event type.
// Customize the logic in each handler function for your specific needs.
//
// For production use with full security features, see examples/node_server.js

const PORT = 8080;

// Type definitions
interface HookEvent {
  version: string;
  event: EventMetadata;
  data: Record<string, any>;
}

interface EventMetadata {
  id: string;
  type: string;
  name: string;
  timestamp: number;
  session_id: string;
  correlation_id?: string;
}

interface HookResponse {
  version?: string;
  decision?: string;
  reason?: string;
  modified_data?: Record<string, any>;
  timestamp: string;
}

// Handler functions for each event type
// Customize these functions to implement your specific logic

function handlePreToolUse(event: HookEvent): HookResponse {
  // Extract tool information
  const toolName = event.data.tool_name || "";
  const toolInput = event.data.tool_input || {};
  const sessionId = event.event.session_id;

  console.log(`[PreToolUse] Tool: ${toolName}, Session: ${sessionId}`);
  console.log(`  Input:`, toolInput);

  // Example: Block dangerous commands
  if (toolName === "Bash") {
    const command = toolInput.command || "";
    // Add your security logic here
    console.log(`  Command: ${command}`);
  }

  // Return decision: "approve", "block", or "modify"
  return {
    version: "1.0",
    // decision: "block",
    // reason: "Dangerous command detected",
    timestamp: new Date().toISOString(),
  };
}

function handlePostToolUse(event: HookEvent): HookResponse {
  // Extract tool information and response
  const toolName = event.data.tool_name || "";
  const toolInput = event.data.tool_input || {};
  const toolResponse = event.data.tool_response || {};
  const sessionId = event.event.session_id;

  console.log(`[PostToolUse] Tool: ${toolName}, Session: ${sessionId}`);
  console.log(`  Input:`, toolInput);
  console.log(`  Response:`, toolResponse);

  // Add your post-execution logic here

  return {
    version: "1.0",
    timestamp: new Date().toISOString(),
  };
}

function handleUserPromptSubmit(event: HookEvent): HookResponse {
  // Extract prompt
  const prompt = event.data.prompt || "";
  const sessionId = event.event.session_id;

  console.log(`[UserPromptSubmit] Session: ${sessionId}`);
  console.log(`  Prompt: ${prompt}`);

  // Add your prompt validation logic here

  return {
    version: "1.0",
    // decision: "block",
    // reason: "Prompt contains sensitive information",
    timestamp: new Date().toISOString(),
  };
}

function handleNotification(event: HookEvent): HookResponse {
  // Extract notification details
  const message = event.data.message || "";
  const title = event.data.title || "";
  const sessionId = event.event.session_id;

  console.log(`[Notification] Session: ${sessionId}`);
  console.log(`  Title: ${title}`);
  console.log(`  Message: ${message}`);

  // Process notification (no decision needed)

  return {
    version: "1.0",
    timestamp: new Date().toISOString(),
  };
}

function handleStop(event: HookEvent): HookResponse {
  // Extract stop information
  const stopHookActive = event.data.stop_hook_active || false;
  const sessionId = event.event.session_id;

  console.log(`[Stop] Session: ${sessionId}`);
  console.log(`  Stop Hook Active: ${stopHookActive}`);

  // Add cleanup logic here

  return {
    version: "1.0",
    // decision: "block",
    // reason: "Cleanup required",
    timestamp: new Date().toISOString(),
  };
}

function handleSubagentStop(event: HookEvent): HookResponse {
  // Extract stop information
  const stopHookActive = event.data.stop_hook_active || false;
  const sessionId = event.event.session_id;

  console.log(`[SubagentStop] Session: ${sessionId}`);
  console.log(`  Stop Hook Active: ${stopHookActive}`);

  // Add subagent cleanup logic here

  return {
    version: "1.0",
    timestamp: new Date().toISOString(),
  };
}

function handlePreCompact(event: HookEvent): HookResponse {
  // Extract compaction details
  const trigger = event.data.trigger || "";
  const customInstructions = event.data.custom_instructions || "";
  const sessionId = event.event.session_id;

  console.log(`[PreCompact] Session: ${sessionId}`);
  console.log(`  Trigger: ${trigger}`);
  if (customInstructions) {
    console.log(`  Instructions: ${customInstructions}`);
  }

  // Process pre-compaction event (no decision needed)

  return {
    version: "1.0",
    timestamp: new Date().toISOString(),
  };
}

const server = Bun.serve({
  port: PORT,
  async fetch(req) {
    const url = new URL(req.url);

    if (url.pathname === "/hook") {
      try {
        // Parse request body
        const body: HookEvent = await req.json();
        
        // Extract event type
        const eventType = body.event?.type || "";
        
        // Route to appropriate handler based on event type
        let response: HookResponse;
        switch (eventType) {
          case "PreToolUse":
            response = handlePreToolUse(body);
            break;
          case "PostToolUse":
            response = handlePostToolUse(body);
            break;
          case "UserPromptSubmit":
            response = handleUserPromptSubmit(body);
            break;
          case "Notification":
            response = handleNotification(body);
            break;
          case "Stop":
            response = handleStop(body);
            break;
          case "SubagentStop":
            response = handleSubagentStop(body);
            break;
          case "PreCompact":
            response = handlePreCompact(body);
            break;
          default:
            console.log(`[Unknown Event] Type: ${eventType}, Session: ${body.event?.session_id}`);
            response = {
              version: "1.0",
              timestamp: new Date().toISOString(),
            };
        }

        return new Response(JSON.stringify(response), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      } catch (error) {
        console.error("Error processing webhook:", error);
        return new Response(
          JSON.stringify({
            error: "Internal server error",
            timestamp: new Date().toISOString(),
          }),
          {
            status: 500,
            headers: { "Content-Type": "application/json" },
          },
        );
      }
    } else {
      return new Response(
        JSON.stringify({
          error: "Not found",
          message: "Webhook endpoint is at /hook",
        }),
        {
          status: 404,
          headers: { "Content-Type": "application/json" },
        },
      );
    }
  },
});

console.log(`🚀 Claude Hooks server listening on http://localhost:${PORT}`);
console.log(`📮 Send webhooks to: http://localhost:${PORT}/hook`);
console.log("\nPress Ctrl+C to stop the server");

// Handle graceful shutdown
process.on("SIGINT", () => {
  console.log("\n👋 Shutting down server...");
  server.stop();
  process.exit(0);
});