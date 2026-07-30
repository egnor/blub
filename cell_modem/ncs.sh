#!/usr/bin/env bash
#
# ncs.sh - run a command inside the pinned NCS toolchain environment, with
# the Serial Modem app directory as the working directory. With no arguments,
# opens an interactive shell in that environment.
#
#   cell_modem/ncs.sh west build     # examples in README.md next to this script
#   cell_modem/ncs.sh                # interactive toolchain shell
#
# Run ./setup (idempotent) first to construct the workspace in dev.tmp/ncs/.
set -euo pipefail

NCS_VERSION="v3.4.0" # keep in sync with setup.py
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(dirname "$HERE")/dev.tmp/ncs"
APP="$TMP/workspace/circuitdojo-ncs-serial-modem/app"

if [[ ! -x "$TMP/bin/nrfutil" || ! -d "$APP" ]]; then
  echo "ERROR: no workspace in $TMP - run $HERE/setup.py first" >&2
  exit 1
fi

# nrfutil self-installs into $NRFUTIL_HOME/bin; make ncs.sh work even
# without mise env activation (though probe-rs still comes via mise's PATH)
export NRFUTIL_HOME="$TMP"
export PATH="$TMP/bin:$PATH"

if [[ $# -eq 0 ]]; then
  exec "$TMP/bin/nrfutil" sdk-manager toolchain launch \
    --ncs-version "$NCS_VERSION" --chdir "$APP" --shell
fi
exec "$TMP/bin/nrfutil" sdk-manager toolchain launch \
  --ncs-version "$NCS_VERSION" --chdir "$APP" -- "$@"
