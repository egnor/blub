#!/usr/bin/env python3
"""
Idempotently builds an nRF Connect SDK (NCS) west workspace under dev.tmp/ncs/
for building Nordic's Serial Modem firmware for the Circuit Dojo nRF9151
Feather. See README.md next to this script for everyday usage.

Everything big lives under dev.tmp/ncs/ (git-ignored):
  nrfutil        - Nordic's CLI (a single static binary)
  nrfutil-home/  - NRFUTIL_HOME (installed nrfutil commands, config, logs)
  toolchains/    - the NCS toolchain bundle (compiler, west, python; ~6 GB)
  cmsis-packs/   - CMSIS device packs, used by pyOCD for flashing
  west/          - the west workspace (manifest/app repo + NCS source, ~5 GB)

Safe to rerun: each step is skipped if already done. The slow steps (toolchain
install, west update) only run when their output is missing; use --update to
force a fresh `west update` (e.g. after editing west.yml).
"""

import argparse
import logging
import ok_logging_setup
import sys
import urllib.request
from ok_subprocess_runner import SubprocessRunner
from pathlib import Path
from subprocess import CalledProcessError

NCS_VERSION = "v3.4.0"  # toolchain bundle; must suit the manifest's west.yml
BOARD = "circuitdojo_feather_nrf9151/nrf9151/ns"
SM_REPO_URL = "https://github.com/circuitdojo/ncs-serial-modem"
SM_REPO_REV = "6dc6a397836465fcff6b5d9de9b604e7f33bb753"
NRFUTIL_URL = (
    "https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/"
    "executables/x86_64-unknown-linux-gnu/nrfutil"
)
PYOCD_TARGET = "nRF9160_xxAA"  # flash-compatible stand-in for the nRF9151

BLUB_DIR = Path(__file__).resolve().parent.parent
NCS_DIR = BLUB_DIR / "dev.tmp" / "ncs"
NRFUTIL_BIN = NCS_DIR / "nrfutil"
WORKSPACE_DIR = NCS_DIR / "west"
MANIFEST_DIR = WORKSPACE_DIR / "circuitdojo-ncs-serial-modem"
VENV_BIN = BLUB_DIR / "dev.tmp" / "python_venv" / "bin"

EXTRA_ENV = {
    "NRFUTIL_HOME": NCS_DIR / "nrfutil-home",
    "CMSIS_PACK_ROOT": NCS_DIR / "cmsis-packs",
    "UV_PROJECT_ENVIRONMENT": VENV_BIN.parent,
}

TOOLCHAIN_PREFIX = [
    *(NRFUTIL_BIN, "toolchain-manager", "launch"),
    *("--ncs-version", NCS_VERSION, "--chdir", WORKSPACE_DIR, "--"),
]

sub = SubprocessRunner(env=EXTRA_ENV)

toolchain_sub = SubprocessRunner(args_prefix=TOOLCHAIN_PREFIX, env=EXTRA_ENV)


def main():
    ok_logging_setup.install()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="force `west update` even if the workspace looks complete",
    )
    args = parser.parse_args()

    NCS_DIR.mkdir(parents=True, exist_ok=True)
    WORKSPACE_DIR.mkdir(exist_ok=True)

    # 1. the nrfutil binary itself (delete dev.tmp/ncs/nrfutil to re-download)
    if not NRFUTIL_BIN.exists():
        logging.info("📥 %s", NRFUTIL_URL)
        tmp = NRFUTIL_BIN.with_suffix(".part")
        urllib.request.urlretrieve(NRFUTIL_URL, tmp)
        tmp.chmod(0o755)
        tmp.rename(NRFUTIL_BIN)

    # 2. the toolchain-manager subcommand (a plugin nrfutil installs)
    try:
        sub(NRFUTIL_BIN, "toolchain-manager", "--version")
    except CalledProcessError:
        sub(NRFUTIL_BIN, "install", "toolchain-manager")

    # 3. the NCS toolchain bundle (compiler, west, python, ...; ~6 GB)
    sub(
        *(NRFUTIL_BIN, "toolchain-manager", "config"),
        *("--set", f"install-dir={NCS_DIR / 'toolchains'}"),
    )
    listing = sub.stdout_lines(NRFUTIL_BIN, "toolchain-manager", "list")
    if NCS_VERSION not in listing:
        sub(
            *(NRFUTIL_BIN, "toolchain-manager", "install"),
            *("--ncs-version", NCS_VERSION),
        )

    # 4. the manifest + app repo (Circuit Dojo's Serial Modem fork; only it
    #    pulls in `nfed`, which has the Feather board definition)
    if not (MANIFEST_DIR / ".git").exists():
        sub("git", "clone", SM_REPO_URL, MANIFEST_DIR)
    head = sub.stdout_lines("git", "-C", MANIFEST_DIR, "rev-parse", "HEAD")[0]
    if head != SM_REPO_REV:
        if sub.stdout_text("git", "-C", MANIFEST_DIR, "status", "--porcelain"):
            sys.exit(
                f"ERROR: {MANIFEST_DIR} has local changes; "
                f"stash/commit them or move it aside, then rerun"
            )
        # fetch the pin explicitly: it may no longer be on any branch tip
        sub("git", "-C", MANIFEST_DIR, "fetch", "origin", SM_REPO_REV)
        sub("git", "-C", MANIFEST_DIR, "checkout", SM_REPO_REV)

    # 5. west workspace init + module checkout (~5 GB on first run)
    if not (WORKSPACE_DIR / ".west").exists():
        toolchain_sub("west", "init", "-l", MANIFEST_DIR.name)
    toolchain_sub("west", "config", "build.board", BOARD)
    if args.update or not (WORKSPACE_DIR / "zephyr").exists():
        toolchain_sub("west", "update")

    # 6. pyOCD (flashes via the Feather's onboard RP2040 CMSIS-DAP probe;
    #    it's a blub dev dependency, so it lives in the project venv)
    if not (VENV_BIN / "pyocd").exists():
        sub("uv", "sync", cwd=BLUB_DIR)
    if not list((NCS_DIR / "cmsis-packs").rglob("*.pack")):
        sub(VENV_BIN / "pyocd", "pack", "install", PYOCD_TARGET)

    logging.info(f"""
✅ NCS workspace ready in {NCS_DIR}
Next steps (see {BLUB_DIR / "cell_modem" / "README.md"}):
  cell_modem/ncs.sh west build   # build Serial Modem for the nRF9151 Feather
  cell_modem/ncs.sh west flash   # flash it over USB (pyOCD / CMSIS-DAP)""")


if __name__ == "__main__":
    main()
