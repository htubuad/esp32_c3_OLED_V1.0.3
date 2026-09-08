import re
import sys
import os

VERSION_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "version.h")

def bump(mode="patch"):
    with open(VERSION_H, "r", encoding="utf-8") as f:
        content = f.read()

    m = re.search(r'#define\s+APP_VERSION\s+"V(\d+)\.(\d+)\.(\d+)"', content)
    if not m:
        print("ERROR: APP_VERSION not found in version.h")
        return 1

    major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    old = f"V{major}.{minor}.{patch}"

    if mode == "major":
        major += 1
        minor = 0
        patch = 0
    elif mode == "minor":
        minor += 1
        patch = 0
    else:
        patch += 1

    new = f"V{major}.{minor}.{patch}"
    new_content = content.replace(f'"{old}"', f'"{new}"')

    with open(VERSION_H, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"Version: {old} -> {new}")
    return 0

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "patch"
    sys.exit(bump(mode))