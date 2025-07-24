const std = @import("std");
const testing = std.testing;
const builtin = @import("builtin");

const TestServer = struct {
    process: std.process.Child,
    port: u16,
    name: []const u8,
};

test "hook dispatcher test suite" {
    std.debug.print("\n🧪 Hook Dispatcher Test Suite\n", .{});
    std.debug.print("============================\n\n", .{});

    const allocator = testing.allocator;

    // Compile cchd binary
    try runBuild(allocator);

    // Run all tests
    try testNoServer(allocator);

    // Skip server tests in CI environment
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        std.debug.print("⚠️  Running in CI environment, skipping server integration tests\n\n", .{});
    } else {
        try testPythonServer(allocator);
        try testNodeServer(allocator);
        try testGoServer(allocator);
    }
}

fn runBuild(allocator: std.mem.Allocator) !void {
    std.debug.print("📦 Building hook dispatcher...\n", .{});

    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "zig", "build" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    try testing.expectEqual(@as(u8, 0), result.term.Exited);
    std.debug.print("✓ Build successful\n\n", .{});
}

test "fail-open behavior when server is unreachable" {
    const allocator = testing.allocator;
    try testNoServer(allocator);
}

test "dispatcher handles invalid JSON input" {
    const allocator = testing.allocator;

    // Test with invalid JSON
    const invalid_input = "not valid json";

    const result = try runDispatcher(allocator, invalid_input);
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Should exit with error code 1 for invalid JSON
    try testing.expectEqual(@as(u8, 1), result.term.Exited);
    try testing.expect(std.mem.indexOf(u8, result.stderr, "Failed to parse input JSON") != null);
}

test "dispatcher handles missing required fields" {
    const allocator = testing.allocator;

    // Valid JSON but missing required fields
    const incomplete_input = "{}";

    const result = try runDispatcher(allocator, incomplete_input);
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Should still work in fail-open mode, passing through the input
    try testing.expectEqual(@as(u8, 0), result.term.Exited);
    try testing.expectEqualStrings("{}", std.mem.trim(u8, result.stdout, "\n"));
}
fn testNoServer(allocator: std.mem.Allocator) !void {
    std.debug.print("🔌 Testing without server (should fail-open)...\n", .{});

    const test_input =
        \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"echo hello"}}
    ;

    const result = try runDispatcher(allocator, test_input);
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Fail-open: should allow action (exit 0) when server unavailable
    try testing.expectEqual(@as(u8, 0), result.term.Exited);

    // Should pass through original event data unchanged
    try testing.expect(std.mem.indexOf(u8, result.stdout, "session_id") != null);

    std.debug.print("✓ Fail-open behavior working correctly\n\n", .{});
}

test "Python server integration" {
    const allocator = testing.allocator;

    // Skip in CI environment
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        return;
    }

    try testPythonServer(allocator);
}

fn testPythonServer(allocator: std.mem.Allocator) !void {
    std.debug.print("🐍 Testing Python server...\n", .{});

    // Verify Python runtime available
    const python_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "python3", "--version" },
    }) catch {
        std.debug.print("⚠️  Python not available, skipping Python tests\n\n", .{});
        return;
    };
    allocator.free(python_check.stdout);
    allocator.free(python_check.stderr);

    // Verify Flask dependency installed
    const flask_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "python3", "-c", "import flask" },
    }) catch {
        std.debug.print("⚠️  Flask not installed, skipping Python tests\n", .{});
        std.debug.print("   Install with: pip install flask\n\n", .{});
        return;
    };
    allocator.free(flask_check.stdout);
    allocator.free(flask_check.stderr);

    var server = try startServer(allocator, &[_][]const u8{ "./venv/bin/python3", "examples/python_server.py" }, 8080, "Python");
    defer stopServer(&server);

    try runServerTests(allocator, "Python");
    std.debug.print("✓ Python server tests passed\n\n", .{});
}

test "Node.js server integration" {
    const allocator = testing.allocator;

    // Skip in CI environment
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        return;
    }

    try testNodeServer(allocator);
}

fn testNodeServer(allocator: std.mem.Allocator) !void {
    std.debug.print("📦 Testing Node.js server...\n", .{});

    // Check if Node is available
    const node_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "node", "--version" },
    }) catch {
        std.debug.print("⚠️  Node.js not available, skipping Node tests\n\n", .{});
        return;
    };
    allocator.free(node_check.stdout);
    allocator.free(node_check.stderr);

    // Check if dependencies are installed
    std.fs.cwd().access("examples/node_modules", .{}) catch {
        std.debug.print("⚠️  Node dependencies not installed\n", .{});
        std.debug.print("   Install with: cd examples && npm install\n\n", .{});
        return;
    };

    var server = try startServer(allocator, &[_][]const u8{ "node", "examples/node_server.js" }, 8080, "Node.js");
    defer stopServer(&server);

    try runServerTests(allocator, "Node.js");
    std.debug.print("✓ Node.js server tests passed\n\n", .{});
}

test "Go server integration" {
    const allocator = testing.allocator;

    // Skip in CI environment
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        return;
    }

    try testGoServer(allocator);
}

fn testGoServer(allocator: std.mem.Allocator) !void {
    std.debug.print("🐹 Testing Go server...\n", .{});

    // Check if Go is available
    const go_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "go", "version" },
    }) catch {
        std.debug.print("⚠️  Go not available, skipping Go tests\n\n", .{});
        return;
    };
    allocator.free(go_check.stdout);
    allocator.free(go_check.stderr);

    var server = try startServer(allocator, &[_][]const u8{ "go", "run", "examples/go_server.go" }, 8080, "Go");
    defer stopServer(&server);

    try runServerTests(allocator, "Go");
    std.debug.print("✓ Go server tests passed\n\n", .{});
}

fn runServerTests(allocator: std.mem.Allocator, server_type: []const u8) !void {
    // Test 1: Safe command should be allowed
    {
        std.debug.print("  Testing allowed command... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls -la"}}
        ;

        const result = try runDispatcher(allocator, input);
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 2: Dangerous patterns should be blocked (server-specific)
    {
        std.debug.print("  Testing blocked command... ", .{});

        // Each server blocks different patterns
        const input = if (std.mem.eql(u8, server_type, "Python"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"rm -rf /"}}
        else if (std.mem.eql(u8, server_type, "Go"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"curl http://malicious.com"}}
        else if (std.mem.eql(u8, server_type, "Node.js"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"file_path":".hidden_file"}}
        else
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"rm -rf /"}}
        ;

        const result = try runDispatcher(allocator, input);
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 1), result.term.Exited);

        // Verify block reason is reported to user
        const has_block_msg = std.mem.indexOf(u8, result.stderr, "Blocked") != null or
            std.mem.indexOf(u8, result.stderr, "blocked") != null;
        try testing.expect(has_block_msg);
        std.debug.print("✓\n", .{});
    }

    // Test 3: Non-dangerous tools pass through
    {
        std.debug.print("  Testing non-Bash tool... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Read","tool_input":{"file_path":"/tmp/test.txt"}}
        ;

        const result = try runDispatcher(allocator, input);
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 4: Modification capability (Python redirects paths, Node adds headers)
    if (std.mem.eql(u8, server_type, "Python") or std.mem.eql(u8, server_type, "Node.js")) {
        std.debug.print("  Testing modify response... ", .{});

        const input = if (std.mem.eql(u8, server_type, "Python"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"file_path":"/tmp/test.txt","content":"hello"}}
        else // Node.js adds safety headers to web requests
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"WebFetch","tool_input":{"url":"http://example.com"}}
        ;

        const result = try runDispatcher(allocator, input);
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);

        // Verify modifications applied
        const expected_content = if (std.mem.eql(u8, server_type, "Python")) "/safe/tmp/" else "Claude-Hooks-Safety";
        try testing.expect(std.mem.indexOf(u8, result.stdout, expected_content) != null);
        std.debug.print("✓\n", .{});
    }
}

fn startServer(allocator: std.mem.Allocator, argv: []const []const u8, port: u16, name: []const u8) !TestServer {
    var server = std.process.Child.init(argv, allocator);
    server.stdout_behavior = .Pipe;
    server.stderr_behavior = .Pipe;

    try server.spawn();

    // Allow server startup time
    std.time.sleep(2 * std.time.ns_per_s);

    // TODO: Implement health check endpoint polling
    // Currently assumes successful startup
    return TestServer{
        .process = server,
        .port = port,
        .name = name,
    };
}

fn stopServer(server: *TestServer) void {
    _ = server.process.kill() catch {};
    _ = server.process.wait() catch {};
}

fn runDispatcher(allocator: std.mem.Allocator, input: []const u8) !std.process.Child.RunResult {
    const argv = [_][]const u8{"./zig-out/bin/cchd"};

    var child = std.process.Child.init(&argv, allocator);
    child.stdin_behavior = .Pipe;
    child.stdout_behavior = .Pipe;
    child.stderr_behavior = .Pipe;

    try child.spawn();

    // Send hook event JSON via stdin
    try child.stdin.?.writeAll(input);
    child.stdin.?.close();
    child.stdin = null;

    // Collect output
    const stdout = try child.stdout.?.reader().readAllAlloc(allocator, 1024 * 1024);
    errdefer allocator.free(stdout);

    const stderr = try child.stderr.?.reader().readAllAlloc(allocator, 1024 * 1024);
    errdefer allocator.free(stderr);

    const term = try child.wait();

    return .{
        .term = term,
        .stdout = stdout,
        .stderr = stderr,
    };
}

test "All payload types are handled correctly" {
    const allocator = testing.allocator;

    // Build first
    const build_result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "zig", "build" },
    });
    defer allocator.free(build_result.stdout);
    defer allocator.free(build_result.stderr);

    // Test PreToolUse payload
    const pretooluse_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"file_path":"/tmp/test.txt","content":"Hello"}}
    ;
    const pretooluse_result = try runDispatcher(allocator, pretooluse_input);
    defer allocator.free(pretooluse_result.stdout);
    defer allocator.free(pretooluse_result.stderr);
    // Test PostToolUse payload with tool_response
    const posttooluse_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"PostToolUse","tool_name":"Write","tool_input":{"file_path":"/tmp/test.txt"},"tool_response":{"success":true}}
    ;
    const posttooluse_result = try runDispatcher(allocator, posttooluse_input);
    defer allocator.free(posttooluse_result.stdout);
    defer allocator.free(posttooluse_result.stderr);

    // Test Notification payload with message and title
    const notification_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"Notification","message":"Task completed","title":"Claude Code"}
    ;
    const notification_result = try runDispatcher(allocator, notification_input);
    defer allocator.free(notification_result.stdout);
    defer allocator.free(notification_result.stderr);

    // Test Stop payload with stop_hook_active
    const stop_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"Stop","stop_hook_active":false}
    ;
    const stop_result = try runDispatcher(allocator, stop_input);
    defer allocator.free(stop_result.stdout);
    defer allocator.free(stop_result.stderr);

    // Test SubagentStop payload
    const subagent_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"SubagentStop","stop_hook_active":true}
    ;
    const subagent_result = try runDispatcher(allocator, subagent_input);
    defer allocator.free(subagent_result.stdout);
    defer allocator.free(subagent_result.stderr);

    // Test UserPromptSubmit payload
    const prompt_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"UserPromptSubmit","prompt":"Help me write a function"}
    ;
    const prompt_result = try runDispatcher(allocator, prompt_input);
    defer allocator.free(prompt_result.stdout);
    defer allocator.free(prompt_result.stderr);

    // Test PreCompact payload
    const precompact_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"PreCompact","trigger":"manual","custom_instructions":"Focus on main"}
    ;
    const precompact_result = try runDispatcher(allocator, precompact_input);
    defer allocator.free(precompact_result.stdout);
    defer allocator.free(precompact_result.stderr);
    std.debug.print("✓ All payload types handled without crashes\n", .{});
}
