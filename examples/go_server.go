package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"time"
)

// Protocol structures matching cchd format
type HookRequest struct {
	Version string          `json:"version"`
	Event   Event           `json:"event"`
	Data    json.RawMessage `json:"data"`
}

type Event struct {
	ID            string `json:"id"`
	Type          string `json:"type"`
	Name          string `json:"name"`
	Timestamp     int64  `json:"timestamp"`
	SessionID     string `json:"session_id"`
	CorrelationID string `json:"correlation_id,omitempty"`
}

type ToolData struct {
	ToolName     string          `json:"tool_name"`
	ToolInput    json.RawMessage `json:"tool_input"`
	ToolResponse json.RawMessage `json:"tool_response,omitempty"`
}

type PromptData struct {
	Prompt string `json:"prompt"`
}

// Server response format
type HookResponse struct {
	Version      string          `json:"version"`
	Decision     string          `json:"decision"`
	Reason       string          `json:"reason,omitempty"`
	ModifiedData json.RawMessage `json:"modified_data,omitempty"`
	Metadata     map[string]any  `json:"metadata,omitempty"`
}

// Request statistics for monitoring
type Stats struct {
	mu            sync.RWMutex
	totalRequests int64
	blockedCount  int64
	modifiedCount int64
	toolCounts    map[string]int64
}

var stats = &Stats{
	toolCounts: make(map[string]int64),
}

// Security patterns and forbidden commands
var (
	// SQL injection detection patterns
	sqlInjectionPatterns = []*regexp.Regexp{
		regexp.MustCompile(`(?i)(union\s+select|drop\s+table|delete\s+from|insert\s+into)`),
		regexp.MustCompile(`(?i)(or\s+1\s*=\s*1|'\s+or\s+')`),
	}

	// Path traversal detection
	pathTraversalPattern = regexp.MustCompile(`\.\.[\\/]`)

	// Network tools that could exfiltrate data
	forbiddenCommands = []string{
		"curl", "wget", "nc", "netcat", "telnet",
	}
)

func main() {
	http.HandleFunc("/hook", handleHook)
	http.HandleFunc("/stats", handleStats)

	log.Println("Claude Hooks Go server starting on :8080")
	if err := http.ListenAndServe(":8080", nil); err != nil {
		log.Fatal(err)
	}
}

func handleHook(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req HookRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	// Update statistics
	stats.mu.Lock()
	stats.totalRequests++
	stats.mu.Unlock()

	log.Printf("Received %s event from session %s", req.Event.Type, req.Event.SessionID)

	// Route to appropriate handler by event type
	var response HookResponse
	switch req.Event.Type {
	case "PreToolUse":
		response = handlePreToolUse(req.Data)
	case "PostToolUse":
		response = handlePostToolUse(req.Data)
	case "UserPromptSubmit":
		response = handleUserPrompt(req.Data)
	default:
		// Allow unrecognized events
		response = HookResponse{
			Version:  "1.0",
			Decision: "allow",
		}
	}

	// Update statistics based on decision
	if response.Decision == "block" {
		stats.mu.Lock()
		stats.blockedCount++
		stats.mu.Unlock()
	} else if response.Decision == "modify" {
		stats.mu.Lock()
		stats.modifiedCount++
		stats.mu.Unlock()
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

func handlePreToolUse(data json.RawMessage) HookResponse {
	var toolData ToolData
	if err := json.Unmarshal(data, &toolData); err != nil {
		return HookResponse{Version: "1.0", Decision: "allow"}
	}

	// Track tool usage statistics
	stats.mu.Lock()
	stats.toolCounts[toolData.ToolName]++
	stats.mu.Unlock()

	// Security checks for Bash commands
	if toolData.ToolName == "Bash" {
		var bashInput struct {
			Command string `json:"command"`
		}
		if err := json.Unmarshal(toolData.ToolInput, &bashInput); err == nil {
			// Block network exfiltration tools
			for _, forbidden := range forbiddenCommands {
				if strings.Contains(bashInput.Command, forbidden) {
					return HookResponse{
						Version:  "1.0",
						Decision: "block",
						Reason:   fmt.Sprintf("Command '%s' is not allowed for security reasons", forbidden),
					}
				}
			}

			// Detect SQL injection attempts
			for _, pattern := range sqlInjectionPatterns {
				if pattern.MatchString(bashInput.Command) {
					return HookResponse{
						Version:  "1.0",
						Decision: "block",
						Reason:   "Potential SQL injection detected in command",
					}
				}
			}
		}
	}

	// Security checks for file operations
	if toolData.ToolName == "Read" || toolData.ToolName == "Write" || toolData.ToolName == "Edit" {
		var fileInput struct {
			FilePath string `json:"file_path"`
			Path     string `json:"path"`
		}
		if err := json.Unmarshal(toolData.ToolInput, &fileInput); err == nil {
			filePath := fileInput.FilePath
			if filePath == "" {
				filePath = fileInput.Path
			}

			// Prevent directory traversal attacks
			if pathTraversalPattern.MatchString(filePath) {
				return HookResponse{
					Version:  "1.0",
					Decision: "block",
					Reason:   "Path traversal attempt detected",
				}
			}

			// Audit trail for file access
			log.Printf("File operation %s on: %s", toolData.ToolName, filePath)
		}
	}
		if err := json.Unmarshal(toolData.ToolInput, &fileInput); err == nil {
			// Check for path traversal
			if pathTraversalPattern.MatchString(fileInput.FilePath) {
				return HookResponse{
					Version:  "1.0",
					Decision: "block",
					Reason:   "Path traversal attempt detected",
				}
			}

			// Example: Sandbox file operations to specific directories
			if !strings.HasPrefix(fileInput.FilePath, "/workspace/") &&
				!strings.HasPrefix(fileInput.FilePath, "/tmp/") {
				// Modify the path to sandbox it
				modifiedInput := map[string]any{
					"file_path": "/workspace" + fileInput.FilePath,
				}
				modifiedData, _ := json.Marshal(map[string]any{
					"tool_input": modifiedInput,
				})

				return HookResponse{
					Version:      "1.0",
					Decision:     "modify",
					ModifiedData: modifiedData,
				}
			}
		}
	}

	return HookResponse{Version: "1.0", Decision: "allow"}
}

func handlePostToolUse(data json.RawMessage) HookResponse {
	// Example: Could analyze tool responses for sensitive data leakage
	return HookResponse{Version: "1.0", Decision: "allow"}
}

func handleUserPrompt(data json.RawMessage) HookResponse {
	var promptData PromptData
	if err := json.Unmarshal(data, &promptData); err != nil {
		return HookResponse{Version: "1.0", Decision: "allow"}
	}

	// Check for SQL injection in prompts
	for _, pattern := range sqlInjectionPatterns {
		if pattern.MatchString(promptData.Prompt) {
			return HookResponse{
				Version:  "1.0",
				Decision: "block",
				Reason:   "Potential SQL injection detected in prompt",
			}
		}
	}

	// Example: Enforce prompt length limits
	if len(promptData.Prompt) > 10000 {
		return HookResponse{
			Version:  "1.0",
			Decision: "block",
			Reason:   "Prompt exceeds maximum length of 10,000 characters",
		}
	}

	return HookResponse{Version: "1.0", Decision: "allow"}
}

func handleStats(w http.ResponseWriter, r *http.Request) {
	stats.mu.RLock()
	defer stats.mu.RUnlock()

	response := map[string]any{
		"total_requests": stats.totalRequests,
		"blocked_count":  stats.blockedCount,
		"modified_count": stats.modifiedCount,
		"tool_counts":    stats.toolCounts,
		"uptime":         time.Since(startTime).String(),
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

var startTime = time.Now()
