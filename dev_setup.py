#!/usr/bin/env python3

import filecmp
import importlib.util
import os
import platform
import shlex
import shutil
import sys
import subprocess
from pathlib import Path

# GENERAL BUILD / DEPENDENCY STRATEGY
# - Use Mise (mise.jdx.dev) to get Python and make a venv (see .mise.toml)
# - Use pip (pypi.org) in that venv to install Python tools

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
if Path(sys.prefix).resolve() != top_dir / "dev.tmp" / "python_venv":
    print(f"💥 Unexpected Python prefix: {sys.prefix}")
    exit(1)

run_shell("mise", "install")

print(f"\n➡️ Git setup")
run_shell("git", "lfs", "install", "--local")
run_shell("git", "lfs", "pull")

print(f"\n➡️ Python setup")
run_shell("python3", "-m", "pip", "install", "-e", ".")

print(f"\n➡️ Arduino setup")
run_shell("arduino-cli", "core", "update-index")
run_shell("arduino-cli", "core", "install", "rp2040:rp2040")
