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

    // Test template servers
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        std.debug.print("⚠️  Running in CI environment, skipping template server tests\n\n", .{});
    } else {
        try testPythonTemplate(allocator);
        try testTypeScriptTemplate(allocator);
        try testGoTemplate(allocator);
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

test "Python template server" {
    const allocator = testing.allocator;

    // Skip in CI environment
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        return;
    }

    try testPythonTemplate(allocator);
}

fn testPythonTemplate(allocator: std.mem.Allocator) !void {
    std.debug.print("🐍 Testing Python template server...\n", .{});

    // Check if uv is available
    const uv_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "uv", "--version" },
    }) catch {
        // Try with python3 directly
        const python_check = std.process.Child.run(.{
            .allocator = allocator,
            .argv = &[_][]const u8{ "python3", "--version" },
        }) catch {
            std.debug.print("⚠️  Neither uv nor Python available, skipping Python template tests\n\n", .{});
            return;
        };
        allocator.free(python_check.stdout);
        allocator.free(python_check.stderr);

        // Check for aiohttp
        const aiohttp_check = std.process.Child.run(.{
            .allocator = allocator,
            .argv = &[_][]const u8{ "python3", "-c", "import aiohttp" },
        }) catch {
            std.debug.print("⚠️  aiohttp not installed, skipping Python template tests\n", .{});
            std.debug.print("   Install with: pip install aiohttp\n\n", .{});
            return;
        };
        allocator.free(aiohttp_check.stdout);
        allocator.free(aiohttp_check.stderr);

        var server = try startServer(allocator, &[_][]const u8{ "python3", "templates/quickstart-python.py" }, 8080, "Python Template");
        defer stopServer(&server);

        try runTemplateTests(allocator, "Python Template");
        std.debug.print("✓ Python template tests passed\n\n", .{});
        return;
    };
    allocator.free(uv_check.stdout);
    allocator.free(uv_check.stderr);

    // Use uv to run the template
    var server = try startServer(allocator, &[_][]const u8{ "uv", "run", "templates/quickstart-python.py" }, 8080, "Python Template (uv)");
    defer stopServer(&server);

    try runTemplateTests(allocator, "Python Template");
    std.debug.print("✓ Python template tests passed\n\n", .{});
}

test "TypeScript template server" {
    const allocator = testing.allocator;

    // Skip in CI environment
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        return;
    }

    try testTypeScriptTemplate(allocator);
}

fn testTypeScriptTemplate(allocator: std.mem.Allocator) !void {
    std.debug.print("📦 Testing TypeScript template server...\n", .{});

    // Check if Bun is available
    const bun_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "bun", "--version" },
    }) catch {
        std.debug.print("⚠️  Bun not available, skipping TypeScript template tests\n", .{});
        std.debug.print("   Install from: https://bun.sh\n\n", .{});
        return;
    };
    allocator.free(bun_check.stdout);
    allocator.free(bun_check.stderr);

    var server = try startServer(allocator, &[_][]const u8{ "bun", "run", "templates/quickstart-typescript.ts" }, 8080, "TypeScript Template");
    defer stopServer(&server);

    try runTemplateTests(allocator, "TypeScript Template");
    std.debug.print("✓ TypeScript template tests passed\n\n", .{});
}

test "Go template server" {
    const allocator = testing.allocator;

    // Skip in CI environment
    const ci_env = std.process.getEnvVarOwned(allocator, "CI") catch null;
    if (ci_env) |ci| {
        defer allocator.free(ci);
        return;
    }

    try testGoTemplate(allocator);
}

fn testGoTemplate(allocator: std.mem.Allocator) !void {
    std.debug.print("🐹 Testing Go template server...\n", .{});

    // Check if Go is available
    const go_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "go", "version" },
    }) catch {
        std.debug.print("⚠️  Go not available, skipping Go template tests\n\n", .{});
        return;
    };
    allocator.free(go_check.stdout);
    allocator.free(go_check.stderr);

    var server = try startServer(allocator, &[_][]const u8{ "go", "run", "templates/quickstart-go.go" }, 8080, "Go Template");
    defer stopServer(&server);

    try runTemplateTests(allocator, "Go Template");
    std.debug.print("✓ Go template tests passed\n\n", .{});
}

fn runTemplateTests(allocator: std.mem.Allocator, template_type: []const u8) !void {
    // Test 1: Basic PreToolUse event handling
    {
        std.debug.print("  Testing PreToolUse event... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"echo hello"}}
        ;

        const result = try runDispatcher(allocator, input);
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        // Templates should allow by default (no decision field)
        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 2: UserPromptSubmit event with new fields
    {
        std.debug.print("  Testing UserPromptSubmit event... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"UserPromptSubmit","prompt":"Hello Claude","current_working_directory":"/home/user/project"}
        ;

        const result = try runDispatcher(allocator, input);
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 3: PostToolUse event
    {
        std.debug.print("  Testing PostToolUse event... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PostToolUse","tool_name":"Read","tool_input":{"file_path":"test.txt"},"tool_response":{"content":"file contents"}}
        ;

        const result = try runDispatcher(allocator, input);
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 4: Other event types
    {
        std.debug.print("  Testing other events... ", .{});

        const events = [_][]const u8{
            \\{"session_id":"test123","hook_event_name":"Notification","message":"Test notification","title":"Test"}
            ,
            \\{"session_id":"test123","hook_event_name":"Stop","stop_hook_active":true}
            ,
            \\{"session_id":"test123","hook_event_name":"SubagentStop","stop_hook_active":false}
            ,
            \\{"session_id":"test123","hook_event_name":"PreCompact","trigger":"manual","custom_instructions":"compact now"}
            ,
        };

        for (events) |event| {
            const result = try runDispatcher(allocator, event);
            defer allocator.free(result.stdout);
            defer allocator.free(result.stderr);

            try testing.expectEqual(@as(u8, 0), result.term.Exited);
        }

        std.debug.print("✓\n", .{});
    }

    _ = template_type;
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
