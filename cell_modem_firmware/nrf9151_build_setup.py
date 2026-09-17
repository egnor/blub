#!/usr/bin/env python3
"""
(Re)builds an nRF Connect SDK (NCS) environment under dev.tmp/nordic/ for
building Nordic Serial Modem firmware for the Circuit Dojo nRF9151 Feather.
See README.md next to this script for everyday usage.

This directory is linked into the workspace as the "manifest repository"
(see west.yml) which pins dependencies, and is also a Zephyr module
(see zephyr/module.yml) with a sysbuild extension (sysbuild/CMakeFiles.txt)
that adds board-specific config overrides.
"""

import argparse
import configparser
import logging
import ok_logging_setup
import os
import tomli_w
from ok_subprocess_runner import run, stdout_text, stdout_json, SubprocessRunner
from pathlib import Path
from subprocess import CalledProcessError

SDK_VERSION = (
    "v3.4.1-rc1"  # toolchain bundle; must suit "nrf" pinned via west.yml
)
SDK_MANAGER_VERSION = "1.16.1"  # nrfutil plugin (nrfutil itself is unpinnable)
BUILD_BOARD = "circuitdojo_feather_nrf9151/nrf9151/ns"


def main():
    ok_logging_setup.install()
    ok_logging_setup.skip_traceback_for(CalledProcessError)
    ok_logging_setup.skip_traceback_for(OSError)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()

    if not (nrfutil_home_env := os.environ.get("NRFUTIL_HOME")):
        ok_logging_setup.exit("$NRFUTIL_HOME not set (check mise?)")

    script_dir = Path(__file__).parent.resolve()
    install_dir = Path(nrfutil_home_env).resolve()
    workspace_dir = install_dir / "workspace"
    manifest_link = workspace_dir / script_dir.name

    logging.info("\n▶️ nRF Connect SDK toolchain")
    nrfutil_list = stdout_json("nrfutil", "list", "--json")["data"]["commands"]
    installed = {c["command"]: c["installed_version"] for c in nrfutil_list}
    if installed.get("sdk-manager") != SDK_MANAGER_VERSION:
        pinned_sdk_manager = f"sdk-manager={SDK_MANAGER_VERSION}"
        run("nrfutil", "install", "--force", pinned_sdk_manager)
    run("nrfutil", "sdk-manager", "config", "install-dir", "set", install_dir)
    toolchain_args = ("nrfutil", "sdk-manager", "toolchain", "install")
    run(*toolchain_args, f"--ncs-version={SDK_VERSION}")

    logging.info("\n▶️ Workspace for west (Zephyr build tool)")
    workspace_dir.mkdir(exist_ok=True, parents=True)
    manifest_link.unlink(missing_ok=True)
    manifest_link.symlink_to(script_dir)

    # Equivalent to "west init -l <dir>", which would resolve the symlink
    # and put the workspace in the wrong place
    west_config_file = workspace_dir / ".west" / "config"
    west_config = configparser.ConfigParser()
    west_config.read(west_config_file)
    west_config.setdefault("manifest", {})
    west_config["manifest"]["path"] = manifest_link.name
    west_config["manifest"]["file"] = "west.yml"
    if "build" in west_config:  # overlays now come via zephyr/module.yml
        west_config["build"].pop("cmake-args", None)
    west_config_file.parent.mkdir(exist_ok=True)
    with open(west_config_file, "w") as f:
        west_config.write(f)

    env_args = ("nrfutil", "sdk-manager", "toolchain", "env")
    env_flags = (f"--ncs-version={SDK_VERSION}", "--json")
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

    mise_prefix = ["mise", "-C", workspace_dir, "exec", "--"]
    run_in_workspace = SubprocessRunner(args_prefix=mise_prefix)
    run_in_workspace("west", "config", "build.board", BUILD_BOARD)
    run_in_workspace("west", "update")  # resets projects to manifest revs
    logging.info(f"\n✅ NCS workspace ready in {install_dir}")


if __name__ == "__main__":
    main()
