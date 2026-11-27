#!/usr/bin/env python3
"""
package_dmg.py - Standalone Dylib Bundler & DMG Packager for MacNewPal

1. Builds Release configuration using xcodebuild.
2. Stages NewPal.app and bundles all required external dynamic libraries (SDL3, FFmpeg closure).
3. Rewrites install names and load paths using install_name_tool to use @rpath.
4. Removes local Homebrew rpaths from the executable.
5. Ad-hoc code-signs all bundled dylibs and the app bundle.
6. Generates a standalone, distributable DMG using create-dmg.
"""

import os
import re
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
BUILD_DIR = os.path.join(PROJECT_DIR, "build")
STAGING_DIR = os.path.join(BUILD_DIR, "staging")
APP_SRC = os.path.join(BUILD_DIR, "Build", "Products", "Release", "NewPal.app")
APP_DST = os.path.join(STAGING_DIR, "NewPal.app")
def read_marketing_version():
    yml = os.path.join(PROJECT_DIR, "project.yml")
    if os.path.exists(yml):
        with open(yml, "r", encoding="utf-8") as f:
            m = re.search(r'MARKETING_VERSION:\s*"([^"]+)"', f.read())
            if m:
                return m.group(1)
    return "1.2.1"


APP_VERSION = read_marketing_version()
DMG_OUTPUT = os.path.join(BUILD_DIR, f"NewPal-{APP_VERSION}.dmg")
APP_ICON = os.path.join(PROJECT_DIR, "Sources", "App", "Resources", "AppIcon.icns")


def run(cmd, check=True):
    print(f"[RUN] {' '.join(cmd)}")
    return subprocess.run(cmd, check=check)


def get_external_deps(file_path):
    out = subprocess.check_output(["otool", "-L", file_path]).decode()
    deps = []
    for line in out.splitlines()[1:]:
        m = re.match(r"\s+([^\s]+)", line)
        if m:
            d = m.group(1)
            if (
                not d.startswith("/System/")
                and not d.startswith("/usr/lib/")
                and not d.startswith("@rpath/")
            ):
                deps.append(d)
    return deps


def main():
    print("=== Step 1: Building Release via xcodebuild ===")
    run([
        "xcodebuild",
        "-project", os.path.join(PROJECT_DIR, "NewPal.xcodeproj"),
        "-scheme", "NewPal",
        "-configuration", "Release",
        "-derivedDataPath", BUILD_DIR,
        "build",
    ])

    if not os.path.exists(APP_SRC):
        print(f"Error: {APP_SRC} does not exist.")
        sys.exit(1)

    print("=== Step 2: Staging Application ===")
    if os.path.exists(STAGING_DIR):
        shutil.rmtree(STAGING_DIR)
    os.makedirs(STAGING_DIR, exist_ok=True)
    shutil.copytree(APP_SRC, APP_DST, symlinks=True)

    frameworks_dir = os.path.join(APP_DST, "Contents", "Frameworks")
    os.makedirs(frameworks_dir, exist_ok=True)
    binary = os.path.join(APP_DST, "Contents", "MacOS", "NewPal")

    print("=== Step 3: Resolving and Bundling Dynamic Libraries ===")
    visited = set()
    to_process = [binary]
    orig_to_target = {}

    while to_process:
        curr = to_process.pop(0)
        for dep in get_external_deps(curr):
            if dep in orig_to_target:
                continue
            real = os.path.realpath(dep)
            target_name = os.path.basename(dep)
            orig_to_target[dep] = target_name
            if real not in visited:
                visited.add(real)
                dst_file = os.path.join(frameworks_dir, target_name)
                if not os.path.exists(dst_file):
                    shutil.copy2(real, dst_file)
                    os.chmod(dst_file, 0o755)
                to_process.append(dst_file)

    print(f"Bundled {len(visited)} external dylibs into Frameworks/")

    print("=== Step 4: Rewriting Install Names and Dependency Paths ===")
    for fname in os.listdir(frameworks_dir):
        fpath = os.path.join(frameworks_dir, fname)
        if not os.path.isfile(fpath) or fname.startswith("."):
            continue
        run(["install_name_tool", "-id", f"@rpath/{fname}", fpath])
        for dep in get_external_deps(fpath):
            if dep in orig_to_target:
                run(["install_name_tool", "-change", dep, f"@rpath/{orig_to_target[dep]}", fpath])

    for dep in get_external_deps(binary):
        if dep in orig_to_target:
            run(["install_name_tool", "-change", dep, f"@rpath/{orig_to_target[dep]}", binary])

    # Strip local Homebrew rpaths from binary
    out = subprocess.check_output(["otool", "-l", binary]).decode()
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line and i + 2 < len(lines):
            m = re.search(r"path\s+([^\s]+)", lines[i + 2])
            if m and m.group(1).startswith("/opt/homebrew"):
                run(["install_name_tool", "-delete_rpath", m.group(1), binary], check=False)

    print("=== Step 5: Code-signing Bundled Artifacts ===")
    for fname in os.listdir(frameworks_dir):
        fpath = os.path.join(frameworks_dir, fname)
        if os.path.isfile(fpath):
            run(["codesign", "--force", "--sign", "-", "--timestamp=none", fpath])
    run(["codesign", "--force", "--sign", "-", "--timestamp=none", APP_DST])

    # Verify zero host leaks
    leaks = []
    for f in os.listdir(frameworks_dir):
        p = os.path.join(frameworks_dir, f)
        chk = subprocess.check_output(["otool", "-L", p]).decode()
        if "/opt/homebrew" in chk:
            leaks.append(f)
    bin_chk = subprocess.check_output(["otool", "-L", binary]).decode()
    if "/opt/homebrew" in bin_chk:
        leaks.append("NewPal binary")

    if leaks:
        print(f"Error: Host path leaks detected in: {leaks}")
        sys.exit(1)
    print("Verification passed: Zero host Homebrew paths detected.")

    print("=== Step 6: Packaging DMG with create-dmg ===")
    if os.path.exists(DMG_OUTPUT):
        os.remove(DMG_OUTPUT)

    create_dmg_cmd = [
        "create-dmg",
        "--volname", "NewPal",
        "--volicon", APP_ICON,
        "--window-pos", "200", "120",
        "--window-size", "660", "400",
        "--icon-size", "128",
        "--text-size", "14",
        "--app-drop-link", "480", "190",
        "--icon", "NewPal.app", "180", "190",
        "--hide-extension", "NewPal.app",
        "--format", "UDZO",
        "--overwrite",
        DMG_OUTPUT,
        STAGING_DIR,
    ]
    run(create_dmg_cmd)

    dmg_size_mb = os.path.getsize(DMG_OUTPUT) / (1024 * 1024)
    print(f"=== Successfully Created DMG: {DMG_OUTPUT} ({dmg_size_mb:.2f} MB) ===")


if __name__ == "__main__":
    main()
