"""
PlatformIO extra script – compute semantic version from conventional commits.

Scans the entire git history. Starting version: 1.0.0.
  - fix:  → patch bump
  - feat: → minor bump (resets patch)
  - BREAKING CHANGE / !: → major bump (resets minor + patch)

Injects -DFIRMWARE_VERSION='"X.Y.Z"' into build flags.
"""

import subprocess
import re

Import("env")


def get_version():
    try:
        result = subprocess.run(
            ["git", "log", "--reverse", "--pretty=format:%s%n%b"],
            capture_output=True, text=True, check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "0.0.0"

    major, minor, patch = 1, 0, 0

    for line in result.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        if "BREAKING CHANGE" in line or re.match(r"^\w+!:", line):
            major += 1
            minor = 0
            patch = 0
        elif line.startswith("feat"):
            minor += 1
            patch = 0
        elif line.startswith("fix"):
            patch += 1

    return f"{major}.{minor}.{patch}"


version = get_version()

try:
    git_hash = subprocess.run(
        ["git", "describe", "--always", "--dirty"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
except (subprocess.CalledProcessError, FileNotFoundError):
    git_hash = "unknown"

print(f"Firmware version: {version} ({git_hash})")
env.Append(CPPDEFINES=[
    ("FIRMWARE_VERSION", f'\\"{version}\\"'),
    ("GIT_HASH", f'\\"{git_hash}\\"'),
])

import os
import glob

vfile = os.path.join(env["PROJECT_DIR"], "version.txt")
try:
    existing = open(vfile).read().strip()
except FileNotFoundError:
    existing = ""
if existing != version:
    with open(vfile, "w") as f:
        f.write(version)
    for cache in glob.glob(os.path.join(env["PROJECT_DIR"], ".pio", "build", "**", "CMakeCache.txt"), recursive=True):
        os.remove(cache)
        print(f"Removed {cache} to force CMake reconfigure")
    print(f"Updated version.txt: {version}")
