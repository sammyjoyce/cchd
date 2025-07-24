const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const yyjson_dep = b.dependency("yyjson", .{
        .target = target,
        .optimize = optimize,
    });

    const yyjson = b.addStaticLibrary(.{
        .name = "yyjson",
        .target = target,
        .optimize = optimize,
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
        .root_source_file = null,
        .target = target,
        .optimize = optimize,
    });

    exe.addCSourceFile(.{
        .file = b.path("src/cchd.c"),
        .flags = &.{ "-Wall", "-Wextra", "-std=c11", "-D_GNU_SOURCE" },
    });

    exe.addIncludePath(yyjson_dep.path("src"));

    exe.linkLibrary(yyjson);
    exe.linkSystemLibrary("curl");
    exe.linkLibC();
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the hook dispatcher");
    run_step.dependOn(&run_cmd.step);

    const test_exe = b.addTest(.{
        .root_source_file = b.path("test.zig"),
        .target = target,
        .optimize = optimize,
    });

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
