// Quick-start template for a Claude Code hooks server using Go.
// Save as quickstart-go.go and run with: go run quickstart-go.go.
//
// This template provides placeholder functions for each hook event type.
// Customize the logic in each handler function for your specific needs.
// The template is designed for rapid prototyping - production deployments
// should add authentication, rate limiting, and proper error handling.
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
// For production use with full security features, see examples/go_server.go.

package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

const PORT = 8080

// CloudEvent represents the incoming CloudEvents format: This structure
// follows the CloudEvents v1.0 specification, providing a standard way to
// describe event data across different systems and protocols.
type CloudEvent struct {
	SpecVersion      string                 `json:"specversion"`
	Type             string                 `json:"type"`
	Source           string                 `json:"source"`
	ID               string                 `json:"id"`
	Time             string                 `json:"time,omitempty"`
	DataContentType  string                 `json:"datacontenttype,omitempty"`
	SessionID        string                 `json:"sessionid,omitempty"`
	CorrelationID    string                 `json:"correlationid,omitempty"`
	Data             map[string]interface{} `json:"data"`
}

// Response represents the webhook response: This structure defines how
// the hook server communicates decisions back to Claude Code, supporting
// both legacy and modern response formats for backward compatibility.
type Response struct {
	Version            string                 `json:"version,omitempty"`
	Decision           string                 `json:"decision,omitempty"`
	Reason             string                 `json:"reason,omitempty"`
	ModifiedData       map[string]interface{} `json:"modified_data,omitempty"`
	HookSpecificOutput *HookSpecificOutput    `json:"hookSpecificOutput,omitempty"`
	Timestamp          string                 `json:"timestamp"`
}

// HookSpecificOutput for modern hook responses (v1.0.59+): This provides
// fine-grained control over permissions and allows hooks to inject additional
// context into Claude's decision-making process.
type HookSpecificOutput struct {
	HookEventName            string `json:"hookEventName"`
	PermissionDecision       string `json:"permissionDecision,omitempty"`
	PermissionDecisionReason string `json:"permissionDecisionReason,omitempty"`
	AdditionalContext        string `json:"additionalContext,omitempty"`
}

// Handler functions for each event type: These functions contain the core
// business logic for processing hook events. Customize these functions to
// implement your specific security policies, logging, or modifications.

func handlePreToolUse(event CloudEvent) Response {
	// Extract tool information from data field: We safely extract fields using
	// type assertions to handle missing or malformed data gracefully.
	toolName, _ := event.Data["tool_name"].(string)
	toolInput, _ := event.Data["tool_input"].(map[string]interface{})
	sessionID := event.SessionID

	fmt.Printf("[PreToolUse] Tool: %s, Session: %s\n", toolName, sessionID)
	fmt.Printf("  Input: %+v\n", toolInput)

	// Example: Block dangerous commands. This demonstrates how to inspect tool
	// inputs and make security decisions based on their content.
	if toolName == "Bash" {
		if command, ok := toolInput["command"].(string); ok {
			// Add your security logic here: Consider checking against allowlists,
	// validating paths, or scanning for sensitive data exposure.
			fmt.Printf("  Command: %s\n", command)
		}
	}

	// Return decision using modern format (v1.0.59+): The response structure
	// supports both legacy and modern formats for maximum compatibility.
	return Response{
		Version: "1.0",
		// Option 1: Modern permission control (preferred)
		// HookSpecificOutput: &HookSpecificOutput{
		//     HookEventName:            "PreToolUse",
		//     PermissionDecision:       "allow", // or "deny" or "ask"
		//     PermissionDecisionReason: "Command is safe",
		// },
		// Option 2: Legacy format
		// Decision: "block",
		// Reason: "Dangerous command detected",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handlePostToolUse(event CloudEvent) Response {
	// Extract tool information and response: PostToolUse events include both
	// the original input and the tool's response, allowing for output validation.
	toolName, _ := event.Data["tool_name"].(string)
	toolInput, _ := event.Data["tool_input"].(map[string]interface{})
	toolResponse, _ := event.Data["tool_response"].(map[string]interface{})
	sessionID := event.SessionID

	fmt.Printf("[PostToolUse] Tool: %s, Session: %s\n", toolName, sessionID)
	fmt.Printf("  Input: %+v\n", toolInput)
	fmt.Printf("  Response: %+v\n", toolResponse)

	// Add your post-execution logic here: Common uses include logging tool
	// outputs, scanning for sensitive data leaks, or triggering follow-up actions.

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handleUserPromptSubmit(event CloudEvent) Response {
	// Extract prompt: UserPromptSubmit events contain the user's raw input
	// before Claude processes it, enabling prompt injection detection.
	prompt, _ := event.Data["prompt"].(string)
	cwd, _ := event.Data["current_working_directory"].(string)
	sessionID := event.SessionID

	fmt.Printf("[UserPromptSubmit] Session: %s\n", sessionID)
	fmt.Printf("  Prompt: %s\n", prompt)
	fmt.Printf("  CWD: %s\n", cwd)

	// Add your prompt validation logic here: Consider checking for prompt
	// injection attempts, PII exposure, or policy violations.

	return Response{
		Version: "1.0",
		// Option 1: Add additional context (v1.0.59+)
		// HookSpecificOutput: &HookSpecificOutput{
		//     HookEventName:     "UserPromptSubmit",
		//     AdditionalContext: "Remember to follow security guidelines",
		// },
		// Option 2: Block the prompt
		// Decision: "block",
		// Reason: "Prompt contains sensitive information",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handleNotification(event CloudEvent) Response {
	// Extract notification details: Notifications are informational events
	// that don't require decisions but can be logged or forwarded.
	message, _ := event.Data["message"].(string)
	title, _ := event.Data["title"].(string)
	sessionID := event.SessionID

	fmt.Printf("[Notification] Session: %s\n", sessionID)
	fmt.Printf("  Title: %s\n", title)
	fmt.Printf("  Message: %s\n", message)

	// Process notification (no decision needed): These events are useful for
	// audit trails, monitoring, or triggering external workflows.

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handleStop(event CloudEvent) Response {
	// Extract stop information: Stop events occur when Claude Code is
	// terminating, allowing for cleanup or session preservation.
	stopHookActive, _ := event.Data["stop_hook_active"].(bool)
	sessionID := event.SessionID

	fmt.Printf("[Stop] Session: %s\n", sessionID)
	fmt.Printf("  Stop Hook Active: %v\n", stopHookActive)

	// Add cleanup logic here: Consider saving session state, closing
	// connections, or notifying external systems of the shutdown.

	return Response{
		Version: "1.0",
		// Decision: "block",
		// Reason: "Cleanup required",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handleSubagentStop(event CloudEvent) Response {
	// Extract stop information: SubagentStop events are similar to Stop events
	// but specific to subagent instances that may have different lifecycles.
	stopHookActive, _ := event.Data["stop_hook_active"].(bool)
	sessionID := event.SessionID

	fmt.Printf("[SubagentStop] Session: %s\n", sessionID)
	fmt.Printf("  Stop Hook Active: %v\n", stopHookActive)

	// Add subagent cleanup logic here: Subagents are separate Claude instances
	// spawned for specific tasks that may need different cleanup procedures.

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handlePreCompact(event CloudEvent) Response {
	// Extract compaction details: PreCompact events fire before Claude
	// compresses conversation history to fit within context limits.
	trigger, _ := event.Data["trigger"].(string)
	customInstructions, _ := event.Data["custom_instructions"].(string)
	sessionID := event.SessionID

	fmt.Printf("[PreCompact] Session: %s\n", sessionID)
	fmt.Printf("  Trigger: %s\n", trigger)
	if customInstructions != "" {
		fmt.Printf("  Instructions: %s\n", customInstructions)
	}

	// Process pre-compaction event (no decision needed): Use this to log
	// what content is being compressed or to preserve important context.

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func webhookHandler(w http.ResponseWriter, r *http.Request) {
	// Read request body: We read the entire body at once since hook payloads
	// are typically small and this simplifies error handling.
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "Failed to read request body", http.StatusBadRequest)
		return
	}

	// Parse JSON (CloudEvents format): The incoming data follows the CloudEvents
	// specification, providing a consistent envelope for all event types.
	var event CloudEvent
	if err := json.Unmarshal(body, &event); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	// Route to appropriate handler based on CloudEvents type: This dispatcher
	// pattern makes it easy to add new event types as Claude Code evolves.
	var response Response
	switch event.Type {
	case "com.claudecode.hook.PreToolUse":
		response = handlePreToolUse(event)
	case "com.claudecode.hook.PostToolUse":
		response = handlePostToolUse(event)
	case "com.claudecode.hook.UserPromptSubmit":
		response = handleUserPromptSubmit(event)
	case "com.claudecode.hook.Notification":
		response = handleNotification(event)
	case "com.claudecode.hook.Stop":
		response = handleStop(event)
	case "com.claudecode.hook.SubagentStop":
		response = handleSubagentStop(event)
	case "com.claudecode.hook.PreCompact":
		response = handlePreCompact(event)
	default:
		fmt.Printf("[Unknown Event] Type: %s, Session: %s\n", event.Type, event.SessionID)
		response = Response{
			Version:   "1.0",
			Timestamp: time.Now().Format(time.RFC3339),
		}
	}

	// Send response: All responses use JSON format to maintain consistency
	// with the CloudEvents input format.
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

func main() {
	// Set up routes: We expose /hook as the main webhook endpoint and provide
	// a helpful error message for requests to other paths.
	http.HandleFunc("/hook", webhookHandler)
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(map[string]string{
			"error":   "Not found",
			"message": "Webhook endpoint is at /hook",
		})
	})

	// Start server: The server listens on localhost by default for security.
	// In production, use proper TLS certificates and authentication.
	fmt.Printf("🚀 Claude Hooks server listening on http://localhost:%d\n", PORT)
	fmt.Printf("📮 Send webhooks to: http://localhost:%d/hook\n", PORT)
	fmt.Println("\nPress Ctrl+C to stop the server")

	// Handle graceful shutdown: This ensures the server can be stopped cleanly
	// with Ctrl+C, allowing any in-flight requests to complete.
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	go func() {
		if err := http.ListenAndServe(fmt.Sprintf(":%d", PORT), nil); err != nil {
			log.Fatal(err)
		}
	}()

	<-sigChan
	fmt.Println("\n👋 Shutting down server...")
}
