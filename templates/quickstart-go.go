// Quick-start template for a Claude Code hooks server using Go
// Save as quickstart-go.go and run with: go run quickstart-go.go
//
// This template provides placeholder functions for each hook event type.
// Customize the logic in each handler function for your specific needs.
//
// For production use with full security features, see examples/go_server.go

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

// HookEvent represents the incoming webhook event structure
type HookEvent struct {
	Version string                 `json:"version"`
	Event   EventMetadata          `json:"event"`
	Data    map[string]interface{} `json:"data"`
}

// EventMetadata contains event information
type EventMetadata struct {
	ID            string `json:"id"`
	Type          string `json:"type"`
	Name          string `json:"name"`
	Timestamp     int64  `json:"timestamp"`
	SessionID     string `json:"session_id"`
	CorrelationID string `json:"correlation_id,omitempty"`
}

// Response represents the webhook response
type Response struct {
	Version            string                 `json:"version,omitempty"`
	Decision           string                 `json:"decision,omitempty"`
	Reason             string                 `json:"reason,omitempty"`
	ModifiedData       map[string]interface{} `json:"modified_data,omitempty"`
	HookSpecificOutput *HookSpecificOutput    `json:"hookSpecificOutput,omitempty"`
	Timestamp          string                 `json:"timestamp"`
}

// HookSpecificOutput for modern hook responses (v1.0.59+)
type HookSpecificOutput struct {
	HookEventName            string `json:"hookEventName"`
	PermissionDecision       string `json:"permissionDecision,omitempty"`
	PermissionDecisionReason string `json:"permissionDecisionReason,omitempty"`
	AdditionalContext        string `json:"additionalContext,omitempty"`
}

// Handler functions for each event type
// Customize these functions to implement your specific logic

func handlePreToolUse(event HookEvent) Response {
	// Extract tool information
	toolName, _ := event.Data["tool_name"].(string)
	toolInput, _ := event.Data["tool_input"].(map[string]interface{})

	fmt.Printf("[PreToolUse] Tool: %s, Session: %s\n", toolName, event.Event.SessionID)
	fmt.Printf("  Input: %+v\n", toolInput)

	// Example: Block dangerous commands
	if toolName == "Bash" {
		if command, ok := toolInput["command"].(string); ok {
			// Add your security logic here
			fmt.Printf("  Command: %s\n", command)
		}
	}

	// Return decision using modern format (v1.0.59+)
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

func handlePostToolUse(event HookEvent) Response {
	// Extract tool information and response
	toolName, _ := event.Data["tool_name"].(string)
	toolInput, _ := event.Data["tool_input"].(map[string]interface{})
	toolResponse, _ := event.Data["tool_response"].(map[string]interface{})

	fmt.Printf("[PostToolUse] Tool: %s, Session: %s\n", toolName, event.Event.SessionID)
	fmt.Printf("  Input: %+v\n", toolInput)
	fmt.Printf("  Response: %+v\n", toolResponse)

	// Add your post-execution logic here

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handleUserPromptSubmit(event HookEvent) Response {
	// Extract prompt
	prompt, _ := event.Data["prompt"].(string)
	cwd, _ := event.Data["current_working_directory"].(string)

	fmt.Printf("[UserPromptSubmit] Session: %s\n", event.Event.SessionID)
	fmt.Printf("  Prompt: %s\n", prompt)
	fmt.Printf("  CWD: %s\n", cwd)

	// Add your prompt validation logic here

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

func handleNotification(event HookEvent) Response {
	// Extract notification details
	message, _ := event.Data["message"].(string)
	title, _ := event.Data["title"].(string)

	fmt.Printf("[Notification] Session: %s\n", event.Event.SessionID)
	fmt.Printf("  Title: %s\n", title)
	fmt.Printf("  Message: %s\n", message)

	// Process notification (no decision needed)

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handleStop(event HookEvent) Response {
	// Extract stop information
	stopHookActive, _ := event.Data["stop_hook_active"].(bool)

	fmt.Printf("[Stop] Session: %s\n", event.Event.SessionID)
	fmt.Printf("  Stop Hook Active: %v\n", stopHookActive)

	// Add cleanup logic here

	return Response{
		Version: "1.0",
		// Decision: "block",
		// Reason: "Cleanup required",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handleSubagentStop(event HookEvent) Response {
	// Extract stop information
	stopHookActive, _ := event.Data["stop_hook_active"].(bool)

	fmt.Printf("[SubagentStop] Session: %s\n", event.Event.SessionID)
	fmt.Printf("  Stop Hook Active: %v\n", stopHookActive)

	// Add subagent cleanup logic here

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func handlePreCompact(event HookEvent) Response {
	// Extract compaction details
	trigger, _ := event.Data["trigger"].(string)
	customInstructions, _ := event.Data["custom_instructions"].(string)

	fmt.Printf("[PreCompact] Session: %s\n", event.Event.SessionID)
	fmt.Printf("  Trigger: %s\n", trigger)
	if customInstructions != "" {
		fmt.Printf("  Instructions: %s\n", customInstructions)
	}

	// Process pre-compaction event (no decision needed)

	return Response{
		Version:   "1.0",
		Timestamp: time.Now().Format(time.RFC3339),
	}
}

func webhookHandler(w http.ResponseWriter, r *http.Request) {
	// Read request body
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "Failed to read request body", http.StatusBadRequest)
		return
	}

	// Parse JSON
	var event HookEvent
	if err := json.Unmarshal(body, &event); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	// Route to appropriate handler based on event type
	var response Response
	switch event.Event.Type {
	case "PreToolUse":
		response = handlePreToolUse(event)
	case "PostToolUse":
		response = handlePostToolUse(event)
	case "UserPromptSubmit":
		response = handleUserPromptSubmit(event)
	case "Notification":
		response = handleNotification(event)
	case "Stop":
		response = handleStop(event)
	case "SubagentStop":
		response = handleSubagentStop(event)
	case "PreCompact":
		response = handlePreCompact(event)
	default:
		fmt.Printf("[Unknown Event] Type: %s, Session: %s\n", event.Event.Type, event.Event.SessionID)
		response = Response{
			Version:   "1.0",
			Timestamp: time.Now().Format(time.RFC3339),
		}
	}

	// Send response
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

func main() {
	// Set up routes
	http.HandleFunc("/hook", webhookHandler)
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(map[string]string{
			"error":   "Not found",
			"message": "Webhook endpoint is at /hook",
		})
	})

	// Start server
	fmt.Printf("🚀 Claude Hooks server listening on http://localhost:%d\n", PORT)
	fmt.Printf("📮 Send webhooks to: http://localhost:%d/hook\n", PORT)
	fmt.Println("\nPress Ctrl+C to stop the server")

	// Handle graceful shutdown
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
