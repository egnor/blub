#!/usr/bin/env bash
#
# ncs - run a command inside the pinned NCS toolchain environment, with the
# Serial Modem app directory as the working directory. With no arguments,
# opens an interactive shell in that environment.
#
#   ncs/ncs west build     # examples in README.md next to this script
#   ncs/ncs                # interactive toolchain shell
#
# Run ./setup (idempotent) first to construct the workspace in dev.tmp/ncs/.
set -euo pipefail

NCS_VERSION="v3.2.1"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(dirname "$HERE")/dev.tmp/ncs"
APP="$TMP/west/circuitdojo-ncs-serial-modem/app"

if [[ ! -x "$TMP/nrfutil" || ! -d "$APP" ]]; then
    echo "ERROR: no workspace in $TMP - run $HERE/setup first" >&2
    exit 1
fi

export NRFUTIL_HOME="$TMP/nrfutil-home"
export CMSIS_PACK_ROOT="$TMP/cmsis-packs"
export PATH="$(dirname "$TMP")/python_venv/bin:$PATH"  # pyocd, for west flash

if [[ $# -eq 0 ]]; then
    exec "$TMP/nrfutil" toolchain-manager launch \
        --ncs-version "$NCS_VERSION" --chdir "$APP" --shell
fi
exec "$TMP/nrfutil" toolchain-manager launch \
    --ncs-version "$NCS_VERSION" --chdir "$APP" -- "$@"
