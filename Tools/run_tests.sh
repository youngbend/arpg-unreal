#!/usr/bin/env bash
#
# Builds the editor target and runs the ARPG automation tests headlessly.
#
#   UE_ROOT=/path/to/UnrealEngine Tools/run_tests.sh            # everything
#   UE_ROOT=/path/to/UnrealEngine Tools/run_tests.sh ARPG.World # one subtree
#
# On Windows, run it from Git Bash or MSYS2 and point UE_ROOT at the install:
#
#   UE_ROOT="/c/Program Files/Epic Games/UE_5.8" Tools/run_tests.sh
#
# WHY A SCRIPT AND NOT A README LINE. The test names, the report path and the
# "-unattended -nullrhi" set are the parts everyone gets subtly wrong, and a
# run configured differently on a laptop and in CI is a run whose disagreements
# nobody can explain. This is the one definition of "the tests pass".
#
# EXIT CODE IS THE REPORT, not the editor's. UnrealEditor-Cmd exits 0 with tests
# failing inside it often enough that trusting it silently green-lights a broken
# branch; the JSON report it writes is the authority, so this parses it.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="${PROJECT_DIR}/arpg.uproject"
FILTER="${1:-ARPG}"
REPORT_DIR="${PROJECT_DIR}/Saved/Automation"

if [[ -z "${UE_ROOT:-}" ]]; then
	echo "UE_ROOT is not set. Point it at an Unreal Engine 5.8 install:" >&2
	echo "  UE_ROOT=~/UnrealEngine Tools/run_tests.sh" >&2
	exit 2
fi

# THE BUILD ENTRY POINT IS NOT UNIFORM ACROSS PLATFORMS, which is what this
# script had wrong. Linux and Mac keep a Build.sh in a per-platform subdirectory;
# a Windows install has neither -- no BatchFiles/Win64/ and no Build.sh anywhere.
# Build.bat sits directly in BatchFiles and is the only entry point shipped
# there. One path derived from ${PLATFORM} cannot say that, so each platform
# names its own. Windows also has to be matched explicitly rather than falling
# out of a default branch, or an unrecognised shell silently tries Win64 paths.
case "$(uname -s)" in
	Linux)
		PLATFORM=Linux
		EDITOR="${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor-Cmd"
		BUILD="${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh"
		;;
	Darwin)
		PLATFORM=Mac
		EDITOR="${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd"
		BUILD="${UE_ROOT}/Engine/Build/BatchFiles/Mac/Build.sh"
		;;
	MINGW*|MSYS*|CYGWIN*)
		PLATFORM=Win64
		EDITOR="${UE_ROOT}/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
		BUILD="${UE_ROOT}/Engine/Build/BatchFiles/Build.bat"
		;;
	*)
		echo "Unrecognised platform '$(uname -s)'. Run this from bash on Linux," >&2
		echo "macOS, or Git Bash / MSYS2 on Windows." >&2
		exit 2
		;;
esac

# -f rather than -x, because a .bat carries no executable bit under MSYS and
# bash runs it through Windows regardless. Checked at all because the bug this
# replaces surfaced as a bare "No such file or directory" from a later line,
# naming a path nobody had asked for.
if [[ ! -f "${BUILD}" ]]; then
	echo "No build script at ${BUILD}." >&2
	echo "Is UE_ROOT (${UE_ROOT}) really an Unreal Engine 5.8 install?" >&2
	exit 2
fi

# WINDOWS PATHS ARE THE OTHER HALF OF THIS. UnrealEditor-Cmd.exe and Build.bat
# are native binaries and cannot read an MSYS path like /c/Users/..., while bash
# goes on using those throughout. Everything handed to a native binary is
# converted once, here, rather than left to the shell's guesswork.
to_native() {
	if [[ "${PLATFORM}" == "Win64" ]]; then
		cygpath -w "$1"
	else
		printf '%s' "$1"
	fi
}

PROJECT_NATIVE="$(to_native "${PROJECT}")"

echo "==> Building arpgEditor (${PLATFORM})"
"${BUILD}" arpgEditor "${PLATFORM}" Development -Project="${PROJECT_NATIVE}" -WaitMutex

echo "==> Running tests matching '${FILTER}'"
rm -rf "${REPORT_DIR}"
mkdir -p "${REPORT_DIR}"

# -nullrhi so this runs without a GPU, which is what makes CI possible at all.
# -unattended and -nopause so a modal dialog cannot hang the run forever.
#
# MSYS2_ARG_CONV_EXCL turns OFF the automatic path rewriting Git Bash applies to
# arguments bound for a native binary. It has to be off: -ExecCmds carries a
# semicolon, which that rewriting reads as a PATH-style separator and mangles.
# The paths it would otherwise have fixed up are already native, via to_native.
# Unset elsewhere, where it is simply an unread variable.
MSYS2_ARG_CONV_EXCL='*' "${EDITOR}" "${PROJECT_NATIVE}" \
	-ExecCmds="Automation RunTests ${FILTER}; Quit" \
	-ReportExportPath="$(to_native "${REPORT_DIR}")" \
	-unattended -nopause -nosplash -nullrhi -stdout -utf8output \
	|| echo "==> Editor exited non-zero; the report below is what decides."

REPORT="${REPORT_DIR}/index.json"
if [[ ! -f "${REPORT}" ]]; then
	echo "==> No report at ${REPORT}. The editor died before running anything." >&2
	exit 1
fi

# python3 is not a given on Windows, where the interpreter is usually `python`
# and `python3` may be the Store stub that launches the Store instead.
PYTHON_BIN="python3"
command -v python3 >/dev/null 2>&1 || PYTHON_BIN="python"

"${PYTHON_BIN}" - "$(to_native "${REPORT}")" <<'PYTHON'
import json, sys

# utf-8-sig: the editor writes the report with a BOM on Windows, which the
# default decoder reads as a stray character before the opening brace.
with open(sys.argv[1], encoding="utf-8-sig") as handle:
    report = json.load(handle)

tests = report.get("tests", [])
failed = [t for t in tests if t.get("state") != "Success"]

for test in failed:
    print("FAIL  {}".format(test.get("fullTestPath", "?")))
    for entry in test.get("entries", []):
        if entry.get("event", {}).get("type") in ("Error", "Warning"):
            print("      {}".format(entry["event"].get("message", "")))

print("\n{} passed, {} failed, {} total".format(
    len(tests) - len(failed), len(failed), len(tests)))

sys.exit(1 if failed or not tests else 0)
PYTHON
