#!/usr/bin/env bun
// Quick-start template for a Claude Code hooks server using Bun.
// Save as quickstart-typescript.ts and run with: bun run quickstart-typescript.ts.
// 
// This template provides placeholder functions for each hook event type.
// Customize the logic in each handler function for your specific needs.
// Bun provides fast startup times and built-in TypeScript support, making
// it ideal for webhook servers that need quick response times.
//
// Events are sent in CloudEvents JSON format (v1.0):
// {
//   "specversion": "1.0",
//   "type": "com.claudecode.hook.PreToolUse",
//   "source": "/claude-code/hooks",
//   "id": "unique-id",
//   "time": "2024-01-15T10:30:00Z",
//   "datacontenttype": "application/json",
//   "sessionid": "session-123",
//   "data": {
//     // Complete unmodified stdin input from Claude.
//   }
// }
//
// For production use with full security features, see examples/node_server.js.

const PORT = 8080;

// Type definitions for CloudEvents format: These interfaces provide type
// safety and better IDE support when working with hook events.
interface CloudEvent {
  specversion: string;
  type: string;
  source: string;
  id: string;
  time?: string;
  datacontenttype?: string;
  sessionid?: string;
  correlationid?: string;
  data: Record<string, any>;
}

interface HookResponse {
  version?: string;
  decision?: string;
  reason?: string;
  modified_data?: Record<string, any>;
  hookSpecificOutput?: {
    hookEventName: string;
    permissionDecision?: string;
    permissionDecisionReason?: string;
    additionalContext?: string;
  };
  timestamp: string;
}

// Handler functions for each event type: These functions contain the core
// business logic for processing hook events. TypeScript's type system helps
// catch errors at compile time rather than runtime.

function handlePreToolUse(event: CloudEvent): HookResponse {
  // Extract tool information from data field: We use default values to handle
  // missing fields gracefully without runtime errors.
  const toolName = event.data.tool_name || "";
  const toolInput = event.data.tool_input || {};
  const sessionId = event.sessionid || "";

  console.log(`[PreToolUse] Tool: ${toolName}, Session: ${sessionId}`);
  console.log(`  Input:`, toolInput);

  // Example: Block dangerous commands. This demonstrates basic command filtering
  // but production systems should implement comprehensive security checks.
  if (toolName === "Bash") {
    const command = toolInput.command || "";
    // Add your security logic here: Consider using regex patterns, command
    // allowlists, or integration with security scanning services.
    console.log(`  Command: ${command}`);
  }

  // Return decision using modern format (v1.0.59+): TypeScript ensures our
  // response structure matches the expected interface.
  return {
    version: "1.0",
    // Option 1: Modern permission control (preferred)
    // hookSpecificOutput: {
    //   hookEventName: "PreToolUse",
    //   permissionDecision: "allow", // or "deny" or "ask"
    //   permissionDecisionReason: "Command is safe"
    // },
    // Option 2: Legacy format
    // decision: "block",
    // reason: "Dangerous command detected",
    timestamp: new Date().toISOString(),
  };
}

function handlePostToolUse(event: CloudEvent): HookResponse {
  // Extract tool information and response: PostToolUse events include both
  // inputs and outputs, enabling comprehensive audit trails.
  const toolName = event.data.tool_name || "";
  const toolInput = event.data.tool_input || {};
  const toolResponse = event.data.tool_response || {};
  const sessionId = event.sessionid || "";

  console.log(`[PostToolUse] Tool: ${toolName}, Session: ${sessionId}`);
  console.log(`  Input:`, toolInput);
  console.log(`  Response:`, toolResponse);

  // Add your post-execution logic here: Common uses include logging outputs,
  // scanning for data leaks, or triggering downstream workflows.

  return {
    version: "1.0",
    timestamp: new Date().toISOString(),
  };
}

function handleUserPromptSubmit(event: CloudEvent): HookResponse {
  // Extract prompt: UserPromptSubmit provides the raw user input before
  // Claude processes it, enabling early intervention for security.
  const prompt = event.data.prompt || "";
  const cwd = event.data.current_working_directory || "";
  const sessionId = event.sessionid || "";

  console.log(`[UserPromptSubmit] Session: ${sessionId}`);
  console.log(`  Prompt: ${prompt}`);
  console.log(`  CWD: ${cwd}`);

  // Add your prompt validation logic here: Consider scanning for injection
  // attacks, PII, or content that violates organizational policies.

  return {
    version: "1.0",
    // Option 1: Add additional context (v1.0.59+)
    // hookSpecificOutput: {
    //   hookEventName: "UserPromptSubmit",
    //   additionalContext: "Remember to follow security guidelines"
    // },
    // Option 2: Block the prompt
    // decision: "block",
    // reason: "Prompt contains sensitive information",
    timestamp: new Date().toISOString(),
  };
}

function handleNotification(event: CloudEvent): HookResponse {
  // Extract notification details: Notifications are informational events
  // that don't affect Claude's behavior but are useful for monitoring.
  const message = event.data.message || "";
  const title = event.data.title || "";
  const sessionId = event.sessionid || "";

  console.log(`[Notification] Session: ${sessionId}`);
  console.log(`  Title: ${title}`);
  console.log(`  Message: ${message}`);

  // Process notification (no decision needed): Use these for audit logs,
  // metrics collection, or integration with monitoring systems.

  return {
    version: "1.0",
    timestamp: new Date().toISOString(),
  };
}

function handleStop(event: CloudEvent): HookResponse {
  // Extract stop information: Stop events signal Claude Code is terminating,
  // providing an opportunity for graceful cleanup.
  const stopHookActive = event.data.stop_hook_active || false;
  const sessionId = event.sessionid || "";

  console.log(`[Stop] Session: ${sessionId}`);
  console.log(`  Stop Hook Active: ${stopHookActive}`);

  // Add cleanup logic here: Save session state, close connections, or
  // notify external systems about the shutdown.

  return {
    version: "1.0",
    // decision: "block",
    // reason: "Cleanup required",
    timestamp: new Date().toISOString(),
  };
}

function handleSubagentStop(event: CloudEvent): HookResponse {
  // Extract stop information: Subagent stop events are similar to regular
  // stop events but specific to spawned subagent instances.
  const stopHookActive = event.data.stop_hook_active || false;
  const sessionId = event.sessionid || "";

  console.log(`[SubagentStop] Session: ${sessionId}`);
  console.log(`  Stop Hook Active: ${stopHookActive}`);

  // Add subagent cleanup logic here: Subagents may have different resource
  // allocations or session data requiring special cleanup procedures.

  return {
    version: "1.0",
    timestamp: new Date().toISOString(),
  };
}

function handlePreCompact(event: CloudEvent): HookResponse {
  // Extract compaction details: PreCompact events fire before Claude reduces
  // conversation history to fit within token limits.
  const trigger = event.data.trigger || "";
  const customInstructions = event.data.custom_instructions || "";
  const sessionId = event.sessionid || "";

  console.log(`[PreCompact] Session: ${sessionId}`);
  console.log(`  Trigger: ${trigger}`);
  if (customInstructions) {
    console.log(`  Instructions: ${customInstructions}`);
  }

  // Process pre-compaction event (no decision needed): Archive important
  // context or extract key information before it's compressed.

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
        // Parse request body (CloudEvents format): Bun's native JSON parsing
        // is optimized for performance with minimal overhead.
        const body: CloudEvent = await req.json();
        
        // Extract CloudEvents type: The type field determines which handler
        // function processes this specific event.
        const eventType = body.type || "";
        const sessionId = body.sessionid || "";
        
        // Route to appropriate handler based on CloudEvents type: This switch
        // statement provides clear routing logic that's easy to extend.
        let response: HookResponse;
        switch (eventType) {
          case "com.claudecode.hook.PreToolUse":
            response = handlePreToolUse(body);
            break;
          case "com.claudecode.hook.PostToolUse":
            response = handlePostToolUse(body);
            break;
          case "com.claudecode.hook.UserPromptSubmit":
            response = handleUserPromptSubmit(body);
            break;
          case "com.claudecode.hook.Notification":
            response = handleNotification(body);
            break;
          case "com.claudecode.hook.Stop":
            response = handleStop(body);
            break;
          case "com.claudecode.hook.SubagentStop":
            response = handleSubagentStop(body);
            break;
          case "com.claudecode.hook.PreCompact":
            response = handlePreCompact(body);
            break;
          default:
            console.log(`[Unknown Event] Type: ${eventType}, Session: ${sessionId}`);
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

// Handle graceful shutdown: This ensures the server stops cleanly when
// interrupted, allowing any in-progress requests to complete.
process.on("SIGINT", () => {
  console.log("\n👋 Shutting down server...");
  server.stop();
  process.exit(0);
});