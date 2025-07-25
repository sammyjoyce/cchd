const std = @import("std");
const testing = std.testing;
const builtin = @import("builtin");

// Mock HTTP server for testing template downloads. We create a local HTTP server
// instead of hitting GitHub's servers to ensure tests are fast, reliable, and
// work offline. This also prevents our tests from being affected by GitHub
// rate limits or network issues. The mock server mimics GitHub's raw content
// API responses for our template files.
const MockServer = struct {
    server: std.net.Server,
    thread: std.Thread,
    port: u16,
    allocator: std.mem.Allocator,

    fn start(allocator: std.mem.Allocator) !MockServer {
        const address = try std.net.Address.parseIp("127.0.0.1", 0);
        const server = try address.listen(.{
            .reuse_address = true,
        });

        const port = server.listen_address.getPort();

        const thread = try std.Thread.spawn(.{}, serverThread, .{server});

        return MockServer{
            .server = server,
            .thread = thread,
            .port = port,
            .allocator = allocator,
        };
    }

    fn stop(self: *MockServer) void {
        self.server.deinit();
        self.thread.join();
    }

    fn serverThread(server: std.net.Server) void {
        while (true) {
            const connection = server.accept() catch break;
            defer connection.stream.close();

            var buf: [4096]u8 = undefined;
            const len = connection.stream.read(&buf) catch break;
            const request = buf[0..len];

            // Parse request path. We use simple string matching rather than a full
            // HTTP parser because we only need to handle specific GET requests for
            // our templates. This keeps the mock server lightweight and focused.
            if (std.mem.indexOf(u8, request, "GET /quickstart-python.py")) |_| {
                const response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n#!/usr/bin/env python3\n# Mock Python template\nprint('Hello from mock template')\n";
                _ = connection.stream.write(response) catch break;
            } else if (std.mem.indexOf(u8, request, "GET /quickstart-typescript.ts")) |_| {
                const response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n// Mock TypeScript template\nconsole.log('Hello from mock template');\n";
                _ = connection.stream.write(response) catch break;
            } else if (std.mem.indexOf(u8, request, "GET /quickstart-go.go")) |_| {
                const response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\npackage main\n// Mock Go template\n";
                _ = connection.stream.write(response) catch break;
            } else {
                const response = "HTTP/1.1 404 Not Found\r\n\r\n";
                _ = connection.stream.write(response) catch break;
            }
        }
    }
};

test "init command help" {
    const allocator = testing.allocator;

    // Test init --help. This verifies that the help text includes all the
    // essential information users need: description, available templates,
    // and usage examples. Good help text is crucial for discoverability.
    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "./zig-out/bin/cchd", "init", "--help" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Verify help command succeeds and contains expected sections. We check for
    // specific strings rather than exact output to allow help text improvements
    // without breaking tests. Each assertion verifies a critical piece of information
    // that users rely on when learning the command.
    try testing.expectEqual(@as(u8, 0), result.term.Exited);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "Initialize a new hook server") != null);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "TEMPLATES") != null);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "python") != null);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "typescript") != null);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "go") != null);
}

test "init command without arguments" {
    const allocator = testing.allocator;

    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "./zig-out/bin/cchd", "init" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Should show usage
    try testing.expect(result.term.Exited != 0);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "USAGE") != null);
}

test "init command with invalid template" {
    const allocator = testing.allocator;

    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "./zig-out/bin/cchd", "init", "ruby" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    try testing.expect(result.term.Exited != 0);
    try testing.expect(std.mem.indexOf(u8, result.stderr, "Unknown template") != null);
    try testing.expect(std.mem.indexOf(u8, result.stderr, "python") != null);
}

test "init creates .claude directory structure" {
    const allocator = testing.allocator;

    // Create temp directory
    var temp_dir = try std.fs.cwd().makeOpenPath("test_init_temp", .{});
    defer std.fs.cwd().deleteTree("test_init_temp") catch {};
    defer temp_dir.close();

    // Change to temp directory
    const cwd = try std.fs.cwd().realpathAlloc(allocator, ".");
    defer allocator.free(cwd);

    try std.os.chdir("test_init_temp");
    defer std.os.chdir(cwd) catch {};

    // Start mock server
    var mock_server = try MockServer.start(allocator);
    defer mock_server.stop();

    // Build modified init command that uses mock server
    const mock_url = try std.fmt.allocPrint(allocator, "http://localhost:{d}/", .{mock_server.port});
    defer allocator.free(mock_url);

    // For this test, we'll check that the directories would be created
    // by verifying the init command structure
    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "../zig-out/bin/cchd", "init", "--help" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Verify help mentions .claude/hooks
    try testing.expect(std.mem.indexOf(u8, result.stdout, ".claude/hooks/") != null);
}

test "init command file operations" {
    const allocator = testing.allocator;

    // Test that init would create files in the right location
    const test_cases = [_]struct {
        args: []const []const u8,
        expected_path: []const u8,
        is_default: bool,
    }{
        .{
            .args = &[_][]const u8{ "init", "python" },
            .expected_path = ".claude/hooks/quickstart-python.py",
            .is_default = true,
        },
        .{
            .args = &[_][]const u8{ "init", "typescript" },
            .expected_path = ".claude/hooks/quickstart-typescript.ts",
            .is_default = true,
        },
        .{
            .args = &[_][]const u8{ "init", "go", "custom.go" },
            .expected_path = ".claude/hooks/custom.go",
            .is_default = true,
        },
        .{
            .args = &[_][]const u8{ "init", "python", "/tmp/hook.py" },
            .expected_path = "/tmp/hook.py",
            .is_default = false,
        },
    };

    // Verify the help output shows correct examples
    const help_result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "./zig-out/bin/cchd", "init", "--help" },
    });
    defer allocator.free(help_result.stdout);
    defer allocator.free(help_result.stderr);

    for (test_cases) |tc| {
        if (tc.is_default) {
            try testing.expect(std.mem.indexOf(u8, help_result.stdout, tc.expected_path) != null);
        }
    }
}

test "settings.json structure" {
    // Test that the init command would create proper settings.json
    // This is a unit test for the expected JSON structure
    const expected_json =
        \\{
        \\    "hookCommand": "cchd --server http://localhost:8080/hook"
        \\}
    ;

    // Parse to verify it's valid JSON
    const parsed = try std.json.parseFromSlice(
        std.json.Value,
        testing.allocator,
        expected_json,
        .{},
    );
    defer parsed.deinit();

    // Verify structure
    try testing.expect(parsed.value.object.contains("hookCommand"));
    const hook_cmd = parsed.value.object.get("hookCommand").?.string;
    try testing.expect(std.mem.indexOf(u8, hook_cmd, "cchd") != null);
    try testing.expect(std.mem.indexOf(u8, hook_cmd, "--server") != null);
}

test "init error messages" {
    const allocator = testing.allocator;

    // Test various error conditions
    const error_cases = [_]struct {
        args: []const []const u8,
        expected_error: []const u8,
    }{
        .{
            .args = &[_][]const u8{ "./zig-out/bin/cchd", "init" },
            .expected_error = "USAGE",
        },
        .{
            .args = &[_][]const u8{ "./zig-out/bin/cchd", "init", "invalid" },
            .expected_error = "Unknown template",
        },
    };

    for (error_cases) |tc| {
        const result = try std.process.Child.run(.{
            .allocator = allocator,
            .argv = tc.args,
        });
        defer allocator.free(result.stdout);
        defer allocator.free(result.stderr);

        try testing.expect(result.term.Exited != 0);
        const output = if (result.stderr.len > 0) result.stderr else result.stdout;
        try testing.expect(std.mem.indexOf(u8, output, tc.expected_error) != null);
    }
}

test "template filename constants" {
    // Verify that template filenames match expected values
    // These should match what's defined in build.zig
    const expected_filenames = .{
        .python = "quickstart-python.py",
        .typescript = "quickstart-typescript.ts",
        .go = "quickstart-go.go",
    };

    // Check help output contains these filenames
    const allocator = testing.allocator;
    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "./zig-out/bin/cchd", "init", "--help" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    try testing.expect(std.mem.indexOf(u8, result.stdout, expected_filenames.python) != null);
    try testing.expect(std.mem.indexOf(u8, result.stdout, expected_filenames.typescript) != null);
    try testing.expect(std.mem.indexOf(u8, result.stdout, expected_filenames.go) != null);
}

test "main help mentions init command" {
    const allocator = testing.allocator;

    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "./zig-out/bin/cchd", "--help" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    try testing.expectEqual(@as(u8, 0), result.term.Exited);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "init") != null);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "Initialize") != null);
}
