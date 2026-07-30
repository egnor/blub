#!/usr/bin/env python3
"""
Idempotently builds an nRF Connect SDK (NCS) west workspace under dev.tmp/ncs/
for building Nordic's Serial Modem firmware for the Circuit Dojo nRF9151
Feather. See README.md next to this script for everyday usage.

Everything big lives under dev.tmp/ncs/ (git-ignored; it is $NRFUTIL_HOME):
  bin/           - nrfutil's self-install + its command plugins (on PATH
                   via mise, which also bootstraps nrfutil & probe-rs)
  downloads/     - the NCS toolchain bundle download cache
  toolchains/    - the NCS toolchain bundle (compiler, west, python)
  tmp/           - temporary downloads etc
  workspace/     - the west workspace (manifest/app repo + NCS source)
  (plus other nrfutil housekeeping: bootstrap/, cache/, config/, logs/, ...)

Safe to rerun: completed steps are quick no-ops.
"""

import argparse
import json
import logging
import ok_logging_setup
import os
from ok_subprocess_runner import run, stdout_text, SubprocessRunner
from pathlib import Path

APP_REPO_URL = "https://github.com/circuitdojo/ncs-serial-modem"
APP_REPO_REV = "6dc6a397836465fcff6b5d9de9b604e7f33bb753"
BOARD = "circuitdojo_feather_nrf9151/nrf9151/ns"
NCS_VERSION = "v3.4.0"  # toolchain bundle; must suit the app
SDK_MANAGER_VERSION = "1.16.1"  # nrfutil plugin (nrfutil itself is unpinnable)


def main():
    ok_logging_setup.install()
    ok_logging_setup.skip_traceback_for(OSError)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()

    if not (nrfutil_home_env := os.environ.get("NRFUTIL_HOME")):
        ok_logging_setup.exit("$NRFUTIL_HOME not set (check mise?)")

    ncs_dir = Path(nrfutil_home_env).resolve()
    workspace_dir = ncs_dir / "workspace"
    app_dir = workspace_dir / "circuitdojo-ncs-serial-modem"

    # 1. the NCS toolchain bundle (compiler, west, python, ...)
    # (`nrfutil install` never up/downgrades an installed plugin, so check)
    listed = json.loads(stdout_text("nrfutil", "list", "--json"))
    installed = {
        c["command"]: c["installed_version"] for c in listed["data"]["commands"]
    }
    if installed.get("sdk-manager") != SDK_MANAGER_VERSION:
        if "sdk-manager" in installed:
            run("nrfutil", "uninstall", "sdk-manager")
        run("nrfutil", "install", f"sdk-manager={SDK_MANAGER_VERSION}")
    run("nrfutil", "sdk-manager", "config", "install-dir", "set", ncs_dir)
    run("nrfutil", "sdk-manager", "install", NCS_VERSION)

    # 2. the app repo at the pinned commit
    if not (app_dir / ".git").is_dir():
        app_dir.parent.mkdir(exist_ok=True, parents=True)
        run("git", "clone", APP_REPO_URL, app_dir)
    if stdout_text("git", "-C", app_dir, "status", "--porcelain"):
        ok_logging_setup.exit("%s has uncommitted changes", app_dir)
    run("git", "-C", app_dir, "fetch", "origin", APP_REPO_REV)
    run("git", "-C", app_dir, "checkout", APP_REPO_REV)

    # 3. build the west workspace around the app repo & its manifest
    sub_with_toolchain = SubprocessRunner(
        args_prefix=[
            *("nrfutil", "sdk-manager", "toolchain", "launch"),
            *(f"--ncs-version={NCS_VERSION}", f"--chdir={workspace_dir}"),
            "--",
        ]
    )
    if not (workspace_dir / ".west").is_dir():
        sub_with_toolchain("west", "init", "-l", app_dir.name)
    sub_with_toolchain("west", "config", "build.board", BOARD)
    # `west flash` = reflash the app image with probe-rs (see README.md);
    # the default pyocd runner can't program UICR, and the non-app domains
    # (b0, mcuboot, provisioning) only flash onto an erased chip anyway
    sub_with_toolchain(
        "west", "config", "alias.flash", "flash --runner probe-rs --domain app"
    )
    sub_with_toolchain("west", "update")

    logging.info(f"""
✅ NCS workspace ready in {ncs_dir}
Next steps (see cell_modem/README.md):
  cell_modem/ncs.sh west build   # build Serial Modem for the nRF9151 Feather
  cell_modem/ncs.sh west flash   # flash it over USB (probe-rs / CMSIS-DAP)""")


if __name__ == "__main__":
    main()
