#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -lt 2 ]; then
	echo "usage: $0 SECONDS COMMAND [ARGUMENT ...]" >&2
	exit 2
fi

seconds=$1
shift
case "$seconds" in
	''|*[!0-9]*)
		echo "timeout must be a non-negative integer number of seconds" >&2
		exit 2
		;;
esac

marker=$(mktemp "${TMPDIR:-/tmp}/dos-c32-timeout.XXXXXX")
rm -f -- "$marker"

"$@" &
command_pid=$!

(
	sleep "$seconds"
	if kill -0 "$command_pid" 2>/dev/null; then
		: > "$marker"
		kill -TERM "$command_pid" 2>/dev/null || true
		sleep 1
		kill -KILL "$command_pid" 2>/dev/null || true
	fi
) &
watchdog_pid=$!

set +e
wait "$command_pid"
command_status=$?
set -e

kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true

if [ -f "$marker" ]; then
	rm -f -- "$marker"
	exit 124
fi

rm -f -- "$marker"
exit "$command_status"
