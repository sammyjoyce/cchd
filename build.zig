const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const version_str = "0.2.0";

    // Template filenames
    const template_python = "quickstart-python.py";
    const template_typescript = "quickstart-typescript.ts";
    const template_go = "quickstart-go.go";

    const yyjson_dep = b.dependency("yyjson", .{
        .target = target,
        .optimize = optimize,
    });

    const aro_dep = b.dependency("aro", .{
        .target = target,
        .optimize = optimize,
    });

    const yyjson = b.addLibrary(.{
        .name = "yyjson",
        .root_module = b.createModule(.{
            .root_source_file = null,
            .target = target,
            .optimize = optimize,
        }),
    });
    yyjson.addCSourceFile(.{
        .file = yyjson_dep.path("src/yyjson.c"),
        .flags = &.{
            "-std=c11",
            "-Wall",
            "-Wextra",
        },
    });
    yyjson.linkLibC();
    yyjson.installHeader(yyjson_dep.path("src/yyjson.h"), "yyjson.h");

    const exe = b.addExecutable(.{
        .name = "cchd",
        .root_module = b.createModule(.{
            .root_source_file = null,
            .target = target,
            .optimize = optimize,
        }),
    });

    // Aro is used for its headers, not as a Zig module: We need Aro's C23
    // standard library headers since many systems don't have C23 support yet.

    // Add all source files
    const c_sources = [_][]const u8{
        "src/main.c",
        "src/core/error.c",
        "src/core/config.c",
        "src/utils/logging.c",
        "src/utils/memory.c",
        "src/utils/colors.c",
        "src/io/input.c",
        "src/io/output.c",
        "src/cli/help.c",
        "src/cli/args.c",
        "src/cli/init.c",
        "src/protocol/json.c",
        "src/protocol/cloudevents.c",
        "src/protocol/validation.c",
        "src/network/http.c",
        "src/network/retry.c",
    };

    for (c_sources) |src| {
        exe.addCSourceFile(.{
            .file = b.path(src),
            .flags = &.{
                "-Wall",
                "-Wextra",
                "-std=c23",
                "-D_GNU_SOURCE",
                b.fmt("-DCCHD_VERSION=\"{s}\"", .{version_str}),
                b.fmt("-DCCHD_TEMPLATE_PYTHON=\"{s}\"", .{template_python}),
                b.fmt("-DCCHD_TEMPLATE_TYPESCRIPT=\"{s}\"", .{template_typescript}),
                b.fmt("-DCCHD_TEMPLATE_GO=\"{s}\"", .{template_go}),
            },
        });
    }

    exe.addIncludePath(yyjson_dep.path("src"));

    exe.linkLibrary(yyjson);
    exe.linkSystemLibrary("curl");
    exe.linkLibC();
    b.installArtifact(exe);

    b.installDirectory(.{
        .source_dir = aro_dep.path("include"),
        .install_dir = .prefix,
        .install_subdir = "include",
    });

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the hook dispatcher");
    run_step.dependOn(&run_cmd.step);

    const test_exe = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("test/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });

    // Test doesn't need Aro: The test code is pure Zig and doesn't require
    // C23 headers, simplifying the test compilation.

    const test_cmd = b.addRunArtifact(test_exe);
    test_cmd.step.dependOn(b.getInstallStep());

    const test_step = b.step("test", "Run comprehensive test suite");
    test_step.dependOn(&test_cmd.step);

    const clean_cmd = b.addSystemCommand(&.{ "rm", "-rf", "zig-out", ".zig-cache" });
    const clean_step = b.step("clean", "Clean build artifacts");
    clean_step.dependOn(&clean_cmd.step);

    const fmt_step = b.step("fmt", "Format all source files");
    const fmt = b.addFmt(.{
        .paths = &.{ "build.zig", "src" },
        .check = false,
    });
    fmt_step.dependOn(&fmt.step);

    const fmt_check_step = b.step("fmt-check", "Check source formatting");
    const fmt_check = b.addFmt(.{
        .paths = &.{ "build.zig", "src" },
        .check = true,
    });
    fmt_check_step.dependOn(&fmt_check.step);

    const check_step = b.step("check", "Run all checks");
    check_step.dependOn(fmt_check_step);
    check_step.dependOn(test_step);
}
