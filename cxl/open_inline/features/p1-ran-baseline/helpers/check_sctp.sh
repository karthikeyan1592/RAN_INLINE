#!/usr/bin/env bash
# check_sctp.sh — P1-R6. Single source shared with p0 (p0-rig-scaffold/helpers/check_sctp.sh's own
# header already states this reuse). This is a thin wrapper, not a copy, so the two features can
# never silently drift apart.
set -u
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../p0-rig-scaffold/helpers" && pwd)/check_sctp.sh" "$@"
