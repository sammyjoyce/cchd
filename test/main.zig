const std = @import("std");
const testing = std.testing;

// Import all test modules. We use pub const declarations to ensure Zig's test
// runner automatically discovers and executes tests from all imported modules.
// This modular structure allows us to organize tests by functionality while
// maintaining a single entry point for 'zig build test'. The separation helps
// isolate test failures and makes the test suite more maintainable.
pub const integration = @import("integration.zig");
pub const init = @import("init.zig");
pub const init_unit = @import("init_unit.zig");

test "cchd test suite" {
    std.debug.print("\n🧪 CCHD Complete Test Suite\n", .{});
    std.debug.print("===========================\n\n", .{});

    // Ensure binary is built. We check for the binary's existence before running
    // tests to provide a better developer experience. If the binary is missing,
    // we automatically build it rather than failing with cryptic "file not found"
    // errors. This makes 'zig build test' work correctly even after a clean checkout.
    const allocator = testing.allocator;
    const binary_path = "./zig-out/bin/cchd";
    const file = std.fs.cwd().openFile(binary_path, .{}) catch {
        std.debug.print("📦 Building cchd binary...\n", .{});
        try runBuild(allocator);
        std.debug.print("✓ Build complete\n\n", .{});
        return;
    };
    file.close();

    std.debug.print("Running all tests...\n\n", .{});

    // The test runner will automatically run all tests from imported modules
    // due to the pub const declarations above. This implicit behavior is a Zig
    // convention: any pub const that references a module with tests will cause
    // those tests to be included in the test run. No explicit test invocation
    // is needed, which prevents accidentally forgetting to run new test modules.
}

fn runBuild(allocator: std.mem.Allocator) !void {
    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "zig", "build" },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    // Check build exit code and provide helpful error output. Build failures during
    // test runs are usually due to compilation errors in recent changes. By showing
    // the stderr output, we help developers quickly identify and fix the issue
    // rather than having to run 'zig build' separately to see the error.
    if (result.term.Exited != 0) {
        std.debug.print("Build failed:\n{s}\n", .{result.stderr});
        return error.BuildFailed;
    }
}
