#!/usr/bin/env python3

import filecmp
import importlib.util
import os
import platform
import shlex
import shutil
import subprocess
from pathlib import Path

# GENERAL BUILD / DEPENDENCY STRATEGY
# - Use Mise (mise.jdx.dev) to get Python and make a venv (see .mise.toml)
# - Use pip (pypi.org) in that venv to install Python tools (platformio, etc.)

def run_shell(*av, **kw):
    av = [str(a) for a in av]
    print(f"🐚 {shlex.join(av)}")
    av = ["mise", "exec", "--", *av] if av[:1] != ["mise"] else av
    return subprocess.run(av, **{"check": True, **kw})

top_dir = Path(__file__).resolve().parent
os.chdir(top_dir)

print(f"➡️ Mise (tool manager) setup")
if not shutil.which("mise"):
    print("🚨 Please install 'mise' (https://mise.jdx.dev/)")
    exit(1)

run_shell("mise", "install")

print(f"\n➡️ Python setup")
run_shell("pip", "install", "-e", ".")

print(f"\n➡️ Git setup")
run_shell("git", "lfs", "install", "--local")
run_shell("git", "lfs", "pull")

print(f"\n➡️ Platformio setup")
if platform.system() == "Linux" and importlib.util.find_spec("platformio"):
    import platformio.fs as pf
    pio_rules_file = Path(pf.get_platformio_udev_rules_path())
    sys_rules_file = Path("/etc/udev/rules.d") / pio_rules_file.name

    print(f"🔌 Checking: {sys_rules_file}")
    if sys_rules_file.is_file() and filecmp.cmp(pio_rules_file, sys_rules_file):
        print("(udev rules already installed)")
    else:
        print(f"Copying: {pio_rules_file}")
        copy_command = ["cp", "-p", str(pio_rules_file), str(sys_rules_file)]
        run_shell("sudo", *copy_command)

print(f"\n😎 Setup complete")
