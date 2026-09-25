import re
import sys
import os

ROOT_CMAKE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "CMakeLists.txt")

def bump(mode="patch"):
    with open(ROOT_CMAKE, "r", encoding="utf-8") as f:
        content = f.read()

    m = re.search(r'project\((\S+)\s+VERSION\s+(\d+)\.(\d+)\.(\d+)\)', content)
    if not m:
        print("ERROR: VERSION not found in CMakeLists.txt")
        return 1

    project_name = m.group(1)
    major, minor, patch = int(m.group(2)), int(m.group(3)), int(m.group(4))
    old = f"{major}.{minor}.{patch}"

    if mode == "major":
        major += 1
        minor = 0
        patch = 0
    elif mode == "minor":
        minor += 1
        patch = 0
    else:
        patch += 1

    new = f"{major}.{minor}.{patch}"
    new_content = content.replace(f"project({project_name} VERSION {old})",
                                  f"project({project_name} VERSION {new})")

    with open(ROOT_CMAKE, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"Version: {old} -> {new}")
    print(f"  Source: {os.path.normpath(ROOT_CMAKE)}")
    print(f"  Next: clean rebuild → bin header + APP_VERSION both = {new}")
    return 0

def set_ver(target):
    with open(ROOT_CMAKE, "r", encoding="utf-8") as f:
        content = f.read()

    m = re.search(r'project\((\S+)\s+VERSION\s+(\d+)\.(\d+)\.(\d+)\)', content)
    if not m:
        print("ERROR: VERSION not found in CMakeLists.txt")
        return 1

    project_name = m.group(1)
    old = f"{m.group(2)}.{m.group(3)}.{m.group(4)}"

    if not re.match(r'^\d+\.\d+\.\d+$', target):
        print(f"ERROR: version must be x.y.z, got '{target}'")
        return 1

    new_content = content.replace(f"project({project_name} VERSION {old})",
                                  f"project({project_name} VERSION {target})")

    with open(ROOT_CMAKE, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"Version: {old} -> {target}")
    print(f"  Source: {os.path.normpath(ROOT_CMAKE)}")
    print(f"  Next: clean rebuild → bin header + APP_VERSION both = {target}")
    return 0

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: version_bump.py <patch|minor|major|set x.y.z>")
        print("       idf.py bump-version          # patch +1")
        print("       idf.py bump-version-minor    # minor +1")
        print("       idf.py bump-version-major    # major +1")
        sys.exit(0)

    arg = sys.argv[1]
    if arg == "set":
        if len(sys.argv) < 3:
            print("ERROR: 'set' requires version, e.g. version_bump.py set 1.8.0")
            sys.exit(1)
        sys.exit(set_ver(sys.argv[2]))
    else:
        sys.exit(bump(arg))