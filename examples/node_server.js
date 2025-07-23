#!/usr/bin/env node
/**
 * Claude Hooks Node.js Server Example
 * 
 * Features:
 * - Rate limiting per session (100 requests/minute)
 * - Audit logging of all hook events
 * - Blocks hidden file creation
 * - Adds security headers to web requests
 */

const express = require('express');
const fs = require('fs').promises;
const path = require('path');

const app = express();
app.use(express.json());

// Rate limiting configuration
const rateLimits = new Map();
const RATE_LIMIT_WINDOW = 60000; // 1 minute window
const MAX_REQUESTS_PER_WINDOW = 100;

// Persistent audit trail
const AUDIT_LOG = path.join(__dirname, 'audit.log');

// Request logging middleware
app.use((req, _res, next) => {
    console.log(`${new Date().toISOString()} - ${req.method} ${req.path}`);
    next();
});

// Main hook endpoint
app.post('/hook', async (req, res) => {
    try {
        const { event, data } = req.body;
        
        // Log all events to audit file
        await logEvent(event, data);
        
        // Check rate limits
        const sessionId = event.session_id;
        if (isRateLimited(sessionId)) {
            return res.json({
                version: '1.0',
                decision: 'block',
                reason: 'Rate limit exceeded. Please wait before trying again.'
            });
        }
        
        // Route events to appropriate handlers
        switch (event.type) {
            case 'PreToolUse':
                return handlePreToolUse(data, res);
            case 'PostToolUse':
                return handlePostToolUse(data, res);
            case 'UserPromptSubmit':
                return handleUserPrompt(data, res);
            default:
                // Allow unhandled events
                return res.json({
                    version: '1.0',
                    decision: 'allow'
                });
        }
    } catch (error) {
        console.error('Error handling request:', error);
        return res.status(500).json({
            version: '1.0',
            decision: 'allow',
            metadata: { error: error.message }
        });
    }
});

function handlePreToolUse(data, res) {
    const { tool_name, tool_input } = data;
    
    // Security enhancement: Add headers to web requests
    if (tool_name === 'WebFetch' || tool_name === 'WebSearch') {
        return res.json({
            version: '1.0',
            decision: 'modify',
            modified_data: {
                tool_input: {
                    ...tool_input,
                    headers: {
                        ...tool_input.headers,
                        'User-Agent': 'Claude-Hooks-Safety/1.0',
                        'X-Request-Source': 'claude-hooks'
                    }
                }
            }
        });
    }
    
    // Policy enforcement: No hidden files
    if (tool_name === 'Write' && tool_input.file_path) {
        const fileName = path.basename(tool_input.file_path);
        
        if (fileName.startsWith('.')) {
            return res.json({
                version: '1.0',
                decision: 'block',
                reason: 'Creating hidden files is not allowed'
            });
        }
        
        // Force backup files to specific directory
        if (fileName.endsWith('.bak') || fileName.endsWith('.backup')) {
            const newPath = path.join('/backups', fileName);
            return res.json({
                version: '1.0',
                decision: 'modify',
                modified_data: {
                    tool_input: {
                        ...tool_input,
                        file_path: newPath
                    }
                }
            });
        }
    }
    
    return res.json({
        version: '1.0',
        decision: 'allow'
    });
}

function handlePostToolUse(data, res) {
    const { tool_name, tool_response } = data;
    
    // Example: Monitor failed operations
    if (tool_response && !tool_response.success) {
        console.warn(`Tool ${tool_name} failed:`, tool_response.error);
        
        // Could send alerts, update metrics, etc.
    }
    
    return res.json({
        version: '1.0',
        decision: 'allow'
    });
}

function handleUserPrompt(data, res) {
    const { prompt } = data;
    
    // Example: Warn about potentially destructive prompts
    const destructiveKeywords = ['delete all', 'remove everything', 'drop database', 'format'];
    const hasDestructive = destructiveKeywords.some(keyword => 
        prompt.toLowerCase().includes(keyword)
    );
    
    if (hasDestructive) {
        return res.json({
            version: '1.0',
            decision: 'block',
            reason: 'This prompt contains potentially destructive operations. Please be more specific about what you want to delete or modify.'
        });
    }
    
    return res.json({
        version: '1.0',
        decision: 'allow'
    });
}

function handleNotification(data, res) {
    const { message } = data;
    
    // Example: Log important notifications
    if (message.includes('permission') || message.includes('waiting')) {
        console.log(`Important notification: ${message}`);
    }
    
    return res.json({
        version: '1.0',
        decision: 'allow'
    });
}

// Rate limiting helper
function isRateLimited(sessionId) {
    const now = Date.now();
    const userLimits = rateLimits.get(sessionId) || { count: 0, resetTime: now + RATE_LIMIT_WINDOW };
    
    if (now > userLimits.resetTime) {
        userLimits.count = 0;
        userLimits.resetTime = now + RATE_LIMIT_WINDOW;
    }
    
    userLimits.count++;
    rateLimits.set(sessionId, userLimits);
    
    return userLimits.count > MAX_REQUESTS_PER_WINDOW;
}

// Audit logging helper
async function logEvent(event, data) {
    const logEntry = {
        timestamp: new Date().toISOString(),
        event_id: event.id,
        event_type: event.type,
        session_id: event.session_id,
        tool_name: data.tool_name,
        summary: getSummary(event.type, data)
    };
    
    try {
        await fs.appendFile(AUDIT_LOG, JSON.stringify(logEntry) + '\n');
    } catch (error) {
        console.error('Failed to write audit log:', error);
    }
}

function getSummary(eventType, data) {
    switch (eventType) {
        case 'PreToolUse':
        case 'PostToolUse':
            return `${data.tool_name} tool`;
        case 'UserPromptSubmit':
            return `Prompt: ${data.prompt?.substring(0, 50)}...`;
        default:
            return eventType;
    }
}

// Start server
const PORT = process.env.PORT || 8080;
app.listen(PORT, () => {
    console.log(`Claude Hooks server listening on port ${PORT}`);
    console.log(`Audit logs will be written to: ${AUDIT_LOG}`);
});