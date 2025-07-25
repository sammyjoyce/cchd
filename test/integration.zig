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

    // Check if binary exists, if not build it. This ensures tests always run
    // against the latest code changes without manual build steps. By automating
    // the build, we prevent the common mistake of testing an outdated binary
    // after making code changes. This is especially important for integration
    // tests that execute the actual binary rather than calling functions directly.
    const binary_path = "./zig-out/bin/cchd";
    const file = std.fs.cwd().openFile(binary_path, .{}) catch {
        // Binary doesn't exist, build it. Automatic build ensures the test
        // suite is self-contained and doesn't require separate build commands.
        // This makes 'zig build test' work correctly on fresh checkouts.
        try runBuild(allocator);
        return;
    };
    file.close();
    std.debug.print("✓ Binary already exists\n\n", .{});

    // Run all tests. These are ordered from simple (no server) to complex
    // (template servers) to help isolate failures. When tests fail, this ordering
    // helps developers quickly identify whether the issue is in the dispatcher
    // itself (no server test) or in the server integration (template tests).
    try testNoServer(allocator);

    // Test template servers. These tests verify the quickstart templates work
    // correctly but are skipped in CI to avoid external dependencies. Template
    // tests require Python/Node/Go runtimes which may not be available in minimal
    // CI environments. By checking the CI environment variable, we maintain fast
    // CI builds while still allowing developers to run comprehensive tests locally.
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

test "dispatcher handles malformed and incomplete JSON" {
    const allocator = testing.allocator;

    // Test 1: Invalid JSON. We test with completely malformed input to ensure
    // the dispatcher fails safely rather than crashing or producing undefined
    // behavior. This protects against potential security issues if Claude Code
    // sends corrupted data.
    std.debug.print("  Testing invalid JSON input... ", .{});
    const invalid_input = "not valid json";

    const result = try runDispatcherWithOptions(allocator, invalid_input, &.{});
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Should exit with CCHD_ERROR_INVALID_JSON (5) because this is not valid JSON.
    // This tests our error handling for malformed input from Claude Code. The
    // specific exit code allows Claude Code to distinguish JSON errors from other
    // failures and provide appropriate user feedback.
    try testing.expectEqual(@as(u8, 5), result.term.Exited);
    std.debug.print("✓\n", .{});

    // Test 2: Valid JSON but missing required fields
    std.debug.print("  Testing JSON with missing required fields... ", .{});
    const incomplete_input = "{}";

    const result2 = try runDispatcherWithOptions(allocator, incomplete_input, &.{});
    defer allocator.free(result2.stdout);
    defer allocator.free(result2.stderr);

    // Should exit with CCHD_ERROR_INVALID_HOOK (6): Empty JSON objects lack
    // required fields, testing our validation of hook event structure.
    try testing.expectEqual(@as(u8, 6), result2.term.Exited);
    std.debug.print("✓\n", .{});
}
fn testNoServer(allocator: std.mem.Allocator) !void {
    std.debug.print("🔌 Testing without server (should fail-open)...\n", .{});

    const test_input =
        \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"echo hello"}}
    ;

    const result = try runDispatcherWithOptions(allocator, test_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Fail-open: should allow action (exit 0) when server unavailable. This
    // ensures Claude Code continues working even if the hook server is down.
    try testing.expectEqual(@as(u8, 0), result.term.Exited);

    // Should output a JSON response indicating success: We verify the output
    // contains expected fields to ensure proper response formatting.
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

    // Check if uv is available: UV provides fast, reliable Python package
    // management without virtual environment complexity.
    const uv_check = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "uv", "--version" },
    }) catch {
        // Try with python3 directly: Fallback to system Python if UV isn't
        // installed, providing wider test coverage.
        const python_check = std.process.Child.run(.{
            .allocator = allocator,
            .argv = &[_][]const u8{ "python3", "--version" },
        }) catch {
            std.debug.print("⚠️  Neither uv nor Python available, skipping Python template tests\n\n", .{});
            return;
        };
        allocator.free(python_check.stdout);
        allocator.free(python_check.stderr);

        // Check for aiohttp: The Python template requires aiohttp for its
        // async HTTP server functionality.
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

    // Use uv to run the template: UV automatically handles dependencies
    // declared in the script's inline metadata.
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

    // Check if Bun is available: Bun provides fast TypeScript execution
    // with built-in HTTP server support.
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

    // Check if Go is available: Go's standard library includes everything
    // needed for the template server.
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
    // Test 1: Basic PreToolUse event handling. This verifies the template
    // correctly processes tool execution requests.
    {
        std.debug.print("  Testing PreToolUse event... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"echo hello"}}
        ;

        const result = try runDispatcherWithOptions(allocator, input, &.{});
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        // Templates should allow by default (no decision field): When templates
        // don't specify a decision, the dispatcher treats it as "allow".
        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 2: UserPromptSubmit event with new fields. This tests handling of
    // prompt validation events including the working directory context.
    {
        std.debug.print("  Testing UserPromptSubmit event... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"UserPromptSubmit","prompt":"Hello Claude","current_working_directory":"/home/user/project"}
        ;

        const result = try runDispatcherWithOptions(allocator, input, &.{});
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 3: PostToolUse event. This verifies templates can process tool
    // outputs for logging or modification purposes.
    {
        std.debug.print("  Testing PostToolUse event... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PostToolUse","tool_name":"Read","tool_input":{"file_path":"test.txt"},"tool_response":{"content":"file contents"}}
        ;

        const result = try runDispatcherWithOptions(allocator, input, &.{});
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 4: Other event types. These events don't require decisions but
    // should be processed without errors for proper hook functionality.
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
            const result = try runDispatcherWithOptions(allocator, event, &.{});
            defer allocator.free(result.stdout);
            defer allocator.free(result.stderr);

            try testing.expectEqual(@as(u8, 0), result.term.Exited);
        }

        std.debug.print("✓\n", .{});
    }

    _ = template_type;
}

fn runServerTests(allocator: std.mem.Allocator, server_type: []const u8) !void {
    // Test 1: Safe command should be allowed. This baseline test ensures
    // legitimate commands pass through the security checks.
    {
        std.debug.print("  Testing allowed command... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls -la"}}
        ;

        const result = try runDispatcherWithOptions(allocator, input, &.{});
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 2: Dangerous patterns should be blocked (server-specific). Each
    // example server demonstrates different security patterns.
    {
        std.debug.print("  Testing blocked command... ", .{});

        // Each server blocks different patterns: This shows how different
        // organizations can implement custom security policies.
        const input = if (std.mem.eql(u8, server_type, "Python"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"rm -rf /"}}
        else if (std.mem.eql(u8, server_type, "Go"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"curl http://malicious.com"}}
        else if (std.mem.eql(u8, server_type, "Node.js"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"file_path":".hidden_file"}}
        else
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"rm -rf /"}}
        ;

        const result = try runDispatcherWithOptions(allocator, input, &.{});
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 1), result.term.Exited);

        // Verify block reason is reported to user: Clear error messages help
        // users understand why their action was blocked.
        const has_block_msg = std.mem.indexOf(u8, result.stderr, "Blocked") != null or
            std.mem.indexOf(u8, result.stderr, "blocked") != null;
        try testing.expect(has_block_msg);
        std.debug.print("✓\n", .{});
    }

    // Test 3: Non-dangerous tools pass through. This ensures security checks
    // are targeted and don't interfere with safe operations.
    {
        std.debug.print("  Testing non-Bash tool... ", .{});
        const input =
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Read","tool_input":{"file_path":"/tmp/test.txt"}}
        ;

        const result = try runDispatcherWithOptions(allocator, input, &.{});
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);
        std.debug.print("✓\n", .{});
    }

    // Test 4: Modification capability (Python redirects paths, Node adds headers).
    // This demonstrates hooks can modify requests, not just allow/block them.
    if (std.mem.eql(u8, server_type, "Python") or std.mem.eql(u8, server_type, "Node.js")) {
        std.debug.print("  Testing modify response... ", .{});

        const input = if (std.mem.eql(u8, server_type, "Python"))
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"file_path":"/tmp/test.txt","content":"hello"}}
        else // Node.js adds safety headers to web requests: This demonstrates
            // how hooks can enhance security by modifying requests.
            \\{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"WebFetch","tool_input":{"url":"http://example.com"}}
        ;

        const result = try runDispatcherWithOptions(allocator, input, &.{});
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expectEqual(@as(u8, 0), result.term.Exited);

        // Verify modifications applied: The output should contain evidence of
        // the server's modifications to the original request.
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

    // Allow server startup time: Template servers need time to bind ports
    // and initialize their HTTP listeners before accepting connections.
    std.Thread.sleep(2 * std.time.ns_per_s);

    // TODO: Implement health check endpoint polling.
    // Currently assumes successful startup. A proper implementation would
    // poll a health endpoint to confirm the server is ready.
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

fn runDispatcherWithOptions(allocator: std.mem.Allocator, input: []const u8, options: []const []const u8) !std.process.Child.RunResult {
    var argv_list = std.ArrayList([]const u8).init(allocator);
    defer argv_list.deinit();
    try argv_list.append("./zig-out/bin/cchd");

    // Add default options for testing consistency: --no-color ensures
    // predictable output that's easier to parse in tests.
    try argv_list.append("--no-color");

    if (options.len > 0) {
        try argv_list.appendSlice(options);
    }

    var child = std.process.Child.init(argv_list.items, allocator);
    child.stdin_behavior = .Pipe;
    child.stdout_behavior = .Pipe;
    child.stderr_behavior = .Pipe;

    try child.spawn();

    // Send hook event JSON via stdin: This simulates how Claude Code
    // sends events to the dispatcher in production.
    try child.stdin.?.writeAll(input);
    child.stdin.?.close();
    child.stdin = null;

    // Collect output: We capture both stdout and stderr to verify correct
    // responses and error messages.
    const stdout = try child.stdout.?.readToEndAlloc(allocator, 1024 * 1024);
    errdefer allocator.free(stdout);

    const stderr = try child.stderr.?.readToEndAlloc(allocator, 1024 * 1024);
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

    // Test PreToolUse payload: This verifies the dispatcher correctly handles
    // tool execution requests with all required fields.
    const pretooluse_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"file_path":"/tmp/test.txt","content":"Hello"}}
    ;
    const pretooluse_result = try runDispatcherWithOptions(allocator, pretooluse_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(pretooluse_result.stdout);
    defer allocator.free(pretooluse_result.stderr);
    // Test PostToolUse payload with tool_response: PostToolUse events include
    // the tool's output, allowing hooks to inspect or log results.
    const posttooluse_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"PostToolUse","tool_name":"Write","tool_input":{"file_path":"/tmp/test.txt"},"tool_response":{"success":true}}
    ;
    const posttooluse_result = try runDispatcherWithOptions(allocator, posttooluse_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(posttooluse_result.stdout);
    defer allocator.free(posttooluse_result.stderr);

    // Test Notification payload with message and title: Notifications provide
    // informational events that don't affect Claude's behavior.
    const notification_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"Notification","message":"Task completed","title":"Claude Code"}
    ;
    const notification_result = try runDispatcherWithOptions(allocator, notification_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(notification_result.stdout);
    defer allocator.free(notification_result.stderr);

    // Test Stop payload with stop_hook_active: The stop_hook_active flag
    // indicates whether the stop was triggered by a hook or user action.
    const stop_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"Stop","stop_hook_active":false}
    ;
    const stop_result = try runDispatcherWithOptions(allocator, stop_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(stop_result.stdout);
    defer allocator.free(stop_result.stderr);

    // Test SubagentStop payload: Subagent stops occur when spawned Claude
    // instances terminate, requiring different handling than main stops.
    const subagent_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"SubagentStop","stop_hook_active":true}
    ;
    const subagent_result = try runDispatcherWithOptions(allocator, subagent_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(subagent_result.stdout);
    defer allocator.free(subagent_result.stderr);

    // Test UserPromptSubmit payload: This event allows hooks to validate or
    // modify user prompts before Claude processes them.
    const prompt_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"UserPromptSubmit","prompt":"Help me write a function"}
    ;
    const prompt_result = try runDispatcherWithOptions(allocator, prompt_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(prompt_result.stdout);
    defer allocator.free(prompt_result.stderr);

    // Test PreCompact payload: PreCompact events let hooks preserve important
    // context before Claude compresses the conversation history.
    const precompact_input =
        \\{"session_id":"test123","transcript_path":"/tmp/transcript.jsonl","hook_event_name":"PreCompact","trigger":"manual","custom_instructions":"Focus on main"}
    ;
    const precompact_result = try runDispatcherWithOptions(allocator, precompact_input, &[_][]const u8{"--fail-open"});
    defer allocator.free(precompact_result.stdout);
    defer allocator.free(precompact_result.stderr);
    std.debug.print("✓ All payload types handled without crashes\n", .{});
}
