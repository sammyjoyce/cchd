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
        .flags = &.{ "-Wall", "-Wextra", "-std=c11" },
    });

    exe.addIncludePath(yyjson_dep.path("src"));

    exe.linkLibrary(yyjson);
    exe.linkSystemLibrary("curl");
    exe.linkLibC();

    if (target.result.os.tag == .linux) {
        exe.linkage = .static;
    }

    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the hook dispatcher");
    run_step.dependOn(&run_cmd.step);

    const test_runner = b.addExecutable(.{
        .name = "test_runner",
        .root_source_file = b.path("test.zig"),
        .target = target,
        .optimize = optimize,
    });

    const test_cmd = b.addRunArtifact(test_runner);
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

    const release_step = b.step("release", "Build release binaries for all targets");

    const targets = [_]std.Target.Query{
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu },
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .gnu },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .x86_64, .os_tag = .macos },
        .{ .cpu_arch = .aarch64, .os_tag = .macos },
        .{ .cpu_arch = .x86_64, .os_tag = .windows },
        .{ .cpu_arch = .aarch64, .os_tag = .windows },
    };

    for (targets) |target_query| {
        const resolved_target = b.resolveTargetQuery(target_query);

        const target_yyjson = b.addStaticLibrary(.{
            .name = "yyjson",
            .target = resolved_target,
            .optimize = .ReleaseSafe,
        });
        target_yyjson.addCSourceFile(.{
            .file = yyjson_dep.path("src/yyjson.c"),
            .flags = &.{
                "-std=c11",
                "-Wall",
                "-Wextra",
            },
        });
        target_yyjson.linkLibC();

        const target_exe = b.addExecutable(.{
            .name = "cchd",
            .root_source_file = null,
            .target = resolved_target,
            .optimize = .ReleaseSafe,
        });

        target_exe.addCSourceFile(.{
            .file = b.path("src/cchd.c"),
            .flags = &.{ "-Wall", "-Wextra", "-std=c11" },
        });

        target_exe.addIncludePath(yyjson_dep.path("src"));
        target_exe.linkLibrary(target_yyjson);
        target_exe.linkSystemLibrary("curl");
        target_exe.linkLibC();

        if (target_query.os_tag == .linux) {
            target_exe.linkage = .static;
        }

        const arch_str = if (target_query.cpu_arch) |arch| @tagName(arch) else "unknown";
        const os_str = if (target_query.os_tag) |os| @tagName(os) else "unknown";
        const abi_str = if (target_query.abi) |abi| @tagName(abi) else "none";

        const triple = b.fmt("{s}-{s}-{s}", .{ arch_str, os_str, abi_str });
        const install_step = b.addInstallArtifact(target_exe, .{
            .dest_dir = .{ .override = .{ .custom = b.fmt("release/{s}", .{triple}) } },
        });

        release_step.dependOn(&install_step.step);
    }

    const check_step = b.step("check", "Run all checks");
    check_step.dependOn(fmt_check_step);
    check_step.dependOn(test_step);
}
