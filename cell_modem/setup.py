#!/usr/bin/env python3
"""
Idempotently builds an nRF Connect SDK (NCS) west workspace under dev.tmp/ncs/
for building Nordic's Serial Modem firmware for the Circuit Dojo nRF9151
Feather. See README.md next to this script for everyday usage.

Safe to rerun: completed steps are quick no-ops.
"""

import argparse
import logging
import ok_logging_setup
import os
import tomli_w
from ok_subprocess_runner import run, stdout_text, stdout_json, SubprocessRunner
from pathlib import Path

APP_REPO_URL = "https://github.com/circuitdojo/ncs-serial-modem"
APP_REPO_REV = "6dc6a397836465fcff6b5d9de9b604e7f33bb753"
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

    logging.info("\n▶️ nRF Connect SDK")
    nrfutil_list = stdout_json("nrfutil", "list", "--json")["data"]["commands"]
    installed = {c["command"]: c["installed_version"] for c in nrfutil_list}
    if installed.get("sdk-manager") != SDK_MANAGER_VERSION:
        pinned_sdk_manager = f"sdk-manager={SDK_MANAGER_VERSION}"
        run("nrfutil", "install", "--force", pinned_sdk_manager)
    run("nrfutil", "sdk-manager", "config", "install-dir", "set", ncs_dir)
    run("nrfutil", "sdk-manager", "install", NCS_VERSION)

    logging.info("\n▶️ Nordic Serial Modem app repo (circuitdojo fork)")
    if not (app_dir / ".git").is_dir():
        app_dir.parent.mkdir(exist_ok=True, parents=True)
        run("git", "clone", APP_REPO_URL, app_dir)
    if stdout_text("git", "-C", app_dir, "status", "--porcelain"):
        ok_logging_setup.exit("%s has uncommitted changes", app_dir)
    run("git", "-C", app_dir, "fetch", "origin", APP_REPO_REV)
    run("git", "-C", app_dir, "checkout", APP_REPO_REV)

    logging.info("\n▶️ Workspace for west (Zephyr OS build tool)")
    env_args = ("nrfutil", "sdk-manager", "toolchain", "env")
    env_flags = (f"--ncs-version={NCS_VERSION}", "--json")
    sdk_vars = stdout_json(*env_args, *env_flags)["data"]["env_variables"]
    sdk_env = {var["key"]: var["value"] for var in sdk_vars}
    sdk_paths = sdk_env.pop("PATH").split(":")
    sys_paths = set(os.environ.get("PATH", "").split(":"))

    mise_t = {"env": sdk_env}
    mise_t["env"]["_"] = {"path": [p for p in sdk_paths if p not in sys_paths]}
    mise_t["env"]["UV_PROJECT_ENVIRONMENT"] = False
    mise_t["settings"] = {"disable_tools": ["python", "uv"]}
    mise_t["settings"]["python"] = {"uv_venv_auto": False}
    with open(workspace_dir / "mise.local.toml", "wb") as f:
        tomli_w.dump(mise_t, f)
    run("mise", "trust", cwd=workspace_dir)

    launch_prefix = [
        *("nrfutil", "sdk-manager", "toolchain", "launch"),
        *(f"--ncs-version={NCS_VERSION}", f"--chdir={workspace_dir}", "--"),
    ]
    run_with_toolchain = SubprocessRunner(args_prefix=launch_prefix)
    if not (workspace_dir / ".west").is_dir():
        run_with_toolchain("west", "init", "-l", app_dir.name)
    build_board = "circuitdojo_feather_nrf9151/nrf9151/ns"
    run_with_toolchain("west", "config", "build.board", build_board)
    # `west flash` = reflash the app image with probe-rs (see README.md);
    # the default pyocd runner can't program UICR, and the non-app domains
    # (b0, mcuboot, provisioning) only flash onto an erased chip anyway
    flash_alias = "flash --runner probe-rs --domain app"
    run_with_toolchain("west", "config", "alias.flash", flash_alias)
    run_with_toolchain("west", "update")

    logging.info(f"""
✅ NCS workspace ready in {ncs_dir}
Next steps (see cell_modem/README.md):
  cd {app_dir}/app
  west build   # build Serial Modem for the nRF9151 Feather
  west flash   # flash it over USB (probe-rs / CMSIS-DAP)""")


if __name__ == "__main__":
    main()
