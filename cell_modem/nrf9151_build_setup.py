#!/usr/bin/env python3
"""
(Re)builds an nRF Connect SDK (NCS) environment under dev.tmp/nordic/ for
building Nordic Serial Modem firmware for the Circuit Dojo nRF9151 Feather.
See README.md next to this script for everyday usage.
"""

import argparse
import logging
import ok_logging_setup
import os
import shlex
import tomli_w
from ok_subprocess_runner import run, stdout_text, stdout_json, SubprocessRunner
from pathlib import Path
from subprocess import CalledProcessError

APP_REPO_URL = "https://github.com/circuitdojo/ncs-serial-modem"
APP_REPO_REV = "6dc6a397836465fcff6b5d9de9b604e7f33bb753"
UPSTREAM_REPO_URL = "https://github.com/nrfconnect/ncs-serial-modem"
UPSTREAM_CHERRY_PICKS = ["refs/pull/381/head"]

# Fixes with no cherry-pickable upstream commit, as (message, unified diff).
# `git apply` fails loudly if the context stops matching, which is what we want:
# once the fork rebases past the fix, the build breaks and the entry comes out.
LOCAL_PATCHES = [
    (
        "app: mqtt: send the #XMQTTMSG header before the payload",
        # Inside sm_at_host_lock() the pipe is not idle, so urc_send_to()
        # queues and the header flushes *after* the topic and payload bytes
        # it describes, which is unparseable. rsp_send_to() sends immediately.
        # Regression from nrfconnect 62061b1; fixed upstream in 7c1cb92 as
        # part of a work-queue refactor we do not want to carry.
        """\
diff --git a/app/src/sm_at_mqtt.c b/app/src/sm_at_mqtt.c
--- a/app/src/sm_at_mqtt.c
+++ b/app/src/sm_at_mqtt.c
@@ -97,7 +97,7 @@ static int handle_mqtt_publish_evt(struct mqtt_client *const c, const struct mqt
 	 * promise is not kept. This deviates from MQTT v3.1.1.
 	 */
 	sm_at_host_lock(ctx.pipe);
-	urc_send_to(ctx.pipe, "\\r\\n#XMQTTMSG: %d,%d\\r\\n",
+	rsp_send_to(ctx.pipe, "\\r\\n#XMQTTMSG: %d,%d\\r\\n",
 		evt->param.publish.message.topic.topic.size,
 		evt->param.publish.message.payload.len);
 	data_send(ctx.pipe, evt->param.publish.message.topic.topic.utf8,
""",
    ),
]

NCS_VERSION = "v3.4.0"  # toolchain bundle; must suit the app
SDK_MANAGER_VERSION = "1.16.1"  # nrfutil plugin (nrfutil itself is unpinnable)


def main():
    ok_logging_setup.install()
    ok_logging_setup.skip_traceback_for(CalledProcessError)
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

    logging.info("\n\n▶️ Nordic Serial Modem app repo (circuitdojo fork)")
    if not (app_dir / ".git").is_dir():
        app_dir.parent.mkdir(exist_ok=True, parents=True)
        run("git", "clone", APP_REPO_URL, app_dir)
    if stdout_text("git", "-C", app_dir, "status", "--porcelain"):
        ok_logging_setup.exit("%s has uncommitted changes", app_dir)
    run("git", "-C", app_dir, "fetch", "origin", APP_REPO_REV)
    run("git", "-C", app_dir, "checkout", "--quiet", "--detach", APP_REPO_REV)
    id = parser.prog or Path(__file__).name
    id_args = ("-c", f"user.name={id}", "-c", f"user.email={id}@invalid")
    for ref in UPSTREAM_CHERRY_PICKS:
        run("git", "-C", app_dir, "fetch", UPSTREAM_REPO_URL, ref)
        run("git", "-C", app_dir, *id_args, "cherry-pick", "FETCH_HEAD")
    for message, diff in LOCAL_PATCHES:
        run("git", "-C", app_dir, "apply", "-", input=diff, text=True)
        run("git", "-C", app_dir, *id_args, "commit", "--all", "-m", message)

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

    mise_prefix = ["mise", "-C", workspace_dir, "exec", "--"]
    run_in_workspace = SubprocessRunner(args_prefix=mise_prefix)
    if not (workspace_dir / ".west").is_dir():
        run_in_workspace("west", "init", "-l", app_dir.name)

    build_board = "circuitdojo_feather_nrf9151/nrf9151/ns"
    run_in_workspace("west", "config", "build.board", build_board)

    script_dir = Path(__file__).parent.resolve()
    cmake_args = {
        "EXTRA_DTC_OVERLAY_FILE": f"{script_dir}/nrf9151_serial_modem.overlay",
        "EXTRA_CONF_FILE": f"{script_dir}/nrf9151_serial_modem.conf",
    }
    cmake_flags = shlex.join(f"-D{k}={v}" for k, v in cmake_args.items())
    run_in_workspace("west", "config", "build.cmake-args", "--", cmake_flags)
    run_in_workspace("west", "update")
    logging.info(f"\n✅ NCS workspace ready in {ncs_dir}")


if __name__ == "__main__":
    main()
