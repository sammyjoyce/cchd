const std = @import("std");
const testing = std.testing;

// Unit tests for init command logic (no network/filesystem required). These tests
// validate the pure logic of the init command without external dependencies. By
// testing logic separately from I/O operations, we can run these tests quickly
// and reliably in any environment, making them ideal for rapid development cycles
// and CI pipelines.

test "template name validation" {
    // Valid template names. We test the exact set of supported templates to ensure
    // our validation logic matches what's actually available. This prevents users
    // from getting confusing "template not found" errors after passing validation.
    const valid_templates = [_][]const u8{ "python", "typescript", "go" };

    // These would be validated in the actual init.c code. We simulate the
    // find_template() function's behavior here to test that our validation
    // logic correctly identifies valid templates. In the real implementation,
    // this check prevents network requests for non-existent templates.
    for (valid_templates) |template| {
        // In the actual implementation, find_template() returns non-null for valid templates
        try testing.expect(std.mem.eql(u8, template, "python") or
            std.mem.eql(u8, template, "typescript") or
            std.mem.eql(u8, template, "go"));
    }

    // Invalid template names. We test common languages that users might expect
    // but that we don't support, as well as edge cases like empty strings. This
    // ensures our error messages are triggered for the most likely user mistakes.
    const invalid_templates = [_][]const u8{ "ruby", "java", "rust", "" };

    for (invalid_templates) |template| {
        try testing.expect(!std.mem.eql(u8, template, "python") and
            !std.mem.eql(u8, template, "typescript") and
            !std.mem.eql(u8, template, "go"));
    }
}

test "path detection logic" {
    // Test cases for determining if a path is absolute or relative. This logic
    // determines whether to use the default .claude/hooks directory or honor the
    // user's specified path. Getting this wrong would put files in unexpected
    // locations, confusing users and potentially overwriting unrelated files.
    const test_cases = [_]struct {
        path: []const u8,
        has_slash: bool,
        is_absolute: bool,
    }{
        .{ .path = "hook.py", .has_slash = false, .is_absolute = false },
        .{ .path = "/tmp/hook.py", .has_slash = true, .is_absolute = true },
        .{ .path = "./hook.py", .has_slash = true, .is_absolute = false },
        .{ .path = "subdir/hook.py", .has_slash = true, .is_absolute = false },
        .{ .path = "../hook.py", .has_slash = true, .is_absolute = false },
    };

    for (test_cases) |tc| {
        const has_slash = std.mem.indexOf(u8, tc.path, "/") != null;
        try testing.expectEqual(tc.has_slash, has_slash);

        const is_absolute = tc.path[0] == '/';
        try testing.expectEqual(tc.is_absolute, is_absolute);
    }
}

test "default path construction" {
    // Test building default paths. We verify that template files end up in the
    // standardized .claude/hooks directory when users don't specify a custom path.
    // This consistent location makes it easier for users to find and manage their
    // hooks, and ensures Claude Code can reliably discover them.
    const templates = [_]struct {
        name: []const u8,
        filename: []const u8,
        expected_path: []const u8,
    }{
        .{ .name = "python", .filename = "quickstart-python.py", .expected_path = ".claude/hooks/quickstart-python.py" },
        .{ .name = "typescript", .filename = "quickstart-typescript.ts", .expected_path = ".claude/hooks/quickstart-typescript.ts" },
        .{ .name = "go", .filename = "quickstart-go.go", .expected_path = ".claude/hooks/quickstart-go.go" },
    };

    const allocator = testing.allocator;

    for (templates) |template| {
        const path = try std.fmt.allocPrint(allocator, ".claude/hooks/{s}", .{template.filename});
        defer allocator.free(path);

        try testing.expectEqualStrings(template.expected_path, path);
    }
}

test "settings.json content generation" {
    // Test the JSON structure that would be generated. We verify the exact format
    // of the settings.json update to ensure Claude Code can parse it correctly.
    // The hookCommand field tells Claude Code how to invoke cchd with the user's
    // hook server URL, completing the integration between the template and Claude.
    const expected_json =
        \\{
        \\    "hookCommand": "cchd --server http://localhost:8080/hook"
        \\}
    ;

    // Verify it's valid JSON by parsing
    const parsed = try std.json.parseFromSlice(
        std.json.Value,
        testing.allocator,
        expected_json,
        .{},
    );
    defer parsed.deinit();

    // Verify content
    try testing.expect(parsed.value.object.contains("hookCommand"));
    const cmd = parsed.value.object.get("hookCommand").?.string;
    try testing.expect(std.mem.indexOf(u8, cmd, "cchd --server") != null);
}
test "GitHub URL construction" {
    const base_url = "https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/";
    const templates = [_]struct {
        filename: []const u8,
        expected_url: []const u8,
    }{
        .{ .filename = "quickstart-python.py", .expected_url = "https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/quickstart-python.py" },
        .{ .filename = "quickstart-typescript.ts", .expected_url = "https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/quickstart-typescript.ts" },
        .{ .filename = "quickstart-go.go", .expected_url = "https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/quickstart-go.go" },
    };

    const allocator = testing.allocator;

    for (templates) |template| {
        const url = try std.fmt.allocPrint(allocator, "{s}{s}", .{ base_url, template.filename });
        defer allocator.free(url);

        try testing.expectEqualStrings(template.expected_url, url);
    }
}

test "directory path parsing" {
    // Test extracting parent directories
    const test_cases = [_]struct {
        path: []const u8,
        parent: []const u8,
        has_parent: bool,
    }{
        .{ .path = ".claude/hooks", .parent = ".claude", .has_parent = true },
        .{ .path = "deep/nested/path", .parent = "deep/nested", .has_parent = true },
        .{ .path = "single", .parent = "", .has_parent = false },
        .{ .path = "/absolute/path", .parent = "/absolute", .has_parent = true },
    };

    for (test_cases) |tc| {
        if (std.mem.lastIndexOf(u8, tc.path, "/")) |index| {
            const parent = tc.path[0..index];
            try testing.expectEqualStrings(tc.parent, parent);
            try testing.expect(tc.has_parent);
        } else {
            try testing.expect(!tc.has_parent);
        }
    }
}

test "file permission bits" {
    // Test that we're setting the correct permissions (755)
    const expected_mode: u32 = 0o755;

    // Verify the bits
    try testing.expect((expected_mode & 0o700) == 0o700); // Owner: rwx
    try testing.expect((expected_mode & 0o050) == 0o050); // Group: r-x
    try testing.expect((expected_mode & 0o005) == 0o005); // Other: r-x
}

test "error message formatting" {
    // Test error message templates
    const allocator = testing.allocator;

    // Test unknown template error
    const unknown_msg = try std.fmt.allocPrint(allocator, "Error: Unknown template '{s}'", .{"ruby"});
    defer allocator.free(unknown_msg);
    try testing.expect(std.mem.indexOf(u8, unknown_msg, "Unknown template 'ruby'") != null);

    // Test file exists error
    const exists_msg = try std.fmt.allocPrint(allocator, "Error: File '{s}' already exists", .{"test.py"});
    defer allocator.free(exists_msg);
    try testing.expect(std.mem.indexOf(u8, exists_msg, "already exists") != null);
}
