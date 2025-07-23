const std = @import("std");
const builtin = @import("builtin");

const TestServer = struct {
    process: std.process.Child,
    port: u16,
    name: []const u8,
};

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    std.debug.print("🧪 Hook Dispatcher Test Suite\n", .{});
    std.debug.print("============================\n\n", .{});

    // Compile cchd binary
    try runBuild(allocator);

    // Verify fail-open behavior when server is unreachable
    try testNoServer(allocator);

    // Test integration with each example server implementation
    try testPythonServer(allocator);
    try testNodeServer(allocator);
    try testGoServer(allocator);

    std.debug.print("\n✅ All tests passed!\n", .{});
}

fn runBuild(allocator: std.mem.Allocator) !void {
    std.debug.print("📦 Building hook dispatcher...\n", .{});

    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "zig", "build" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    if (result.term.Exited != 0) {
        std.debug.print("❌ Build failed:\n{s}\n", .{result.stderr});
        return error.BuildFailed;
    }

    std.debug.print("✓ Build successful\n\n", .{});
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
    if (result.term.Exited != 0) {
        std.debug.print("❌ Expected exit code 0, got {}\n", .{result.term.Exited});
        return error.TestFailed;
    }

    // Should pass through original event data unchanged
    if (std.mem.indexOf(u8, result.stdout, "session_id") == null) {
        std.debug.print("❌ Expected output to contain original input\n", .{});
        return error.TestFailed;
    }

    std.debug.print("✓ Fail-open behavior working correctly\n\n", .{});
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

    var server = try startServer(allocator, &[_][]const u8{ "python3", "examples/python_server.py" }, 8080, "Python");
    defer stopServer(&server);

    try runServerTests(allocator, "Python");
    std.debug.print("✓ Python server tests passed\n\n", .{});
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

        if (result.term.Exited != 0) {
            std.debug.print("❌\n    Expected exit code 0, got {}\n", .{result.term.Exited});
            return error.TestFailed;
        }
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

        if (result.term.Exited != 1) {
            std.debug.print("❌\n    Expected exit code 1, got {}\n", .{result.term.Exited});
            std.debug.print("    Server type: {s}\n", .{server_type});
            std.debug.print("    stderr: {s}\n", .{result.stderr});
            return error.TestFailed;
        }

        // Verify block reason is reported to user
        if (std.mem.indexOf(u8, result.stderr, "Blocked") == null and
            std.mem.indexOf(u8, result.stderr, "blocked") == null)
        {
            std.debug.print("❌\n    Expected block reason in stderr\n", .{});
            return error.TestFailed;
        }
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

        if (result.term.Exited != 0) {
            std.debug.print("❌\n    Expected exit code 0, got {}\n", .{result.term.Exited});
            return error.TestFailed;
        }
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

        if (result.term.Exited != 0) {
            std.debug.print("❌\n    Expected exit code 0 for modify, got {}\n", .{result.term.Exited});
            return error.TestFailed;
        }

        // Verify modifications applied
        if (std.mem.indexOf(u8, result.stdout, if (std.mem.eql(u8, server_type, "Python")) "/safe/tmp/" else "Claude-Hooks-Safety") == null) {
            std.debug.print("❌\n    Expected modified output not found\n", .{});
            std.debug.print("    stdout: {s}\n", .{result.stdout});
            return error.TestFailed;
        }
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
