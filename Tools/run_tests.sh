#!/usr/bin/env bash
#
# Builds the editor target and runs the ARPG automation tests headlessly.
#
#   UE_ROOT=/path/to/UnrealEngine Tools/run_tests.sh            # everything
#   UE_ROOT=/path/to/UnrealEngine Tools/run_tests.sh ARPG.World # one subtree
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

case "$(uname -s)" in
	Linux)  PLATFORM=Linux;  EDITOR="${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor-Cmd" ;;
	Darwin) PLATFORM=Mac;    EDITOR="${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd" ;;
	*)      PLATFORM=Win64;  EDITOR="${UE_ROOT}/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" ;;
esac

BUILD="${UE_ROOT}/Engine/Build/BatchFiles/${PLATFORM}/Build.sh"
[[ -x "${BUILD}" ]] || BUILD="${UE_ROOT}/Engine/Build/BatchFiles/Build.sh"

echo "==> Building arpgEditor (${PLATFORM})"
"${BUILD}" arpgEditor "${PLATFORM}" Development -Project="${PROJECT}" -WaitMutex

echo "==> Running tests matching '${FILTER}'"
rm -rf "${REPORT_DIR}"
mkdir -p "${REPORT_DIR}"

# -nullrhi so this runs without a GPU, which is what makes CI possible at all.
# -unattended and -nopause so a modal dialog cannot hang the run forever.
"${EDITOR}" "${PROJECT}" \
	-ExecCmds="Automation RunTests ${FILTER}; Quit" \
	-ReportExportPath="${REPORT_DIR}" \
	-unattended -nopause -nosplash -nullrhi -stdout -utf8output \
	|| echo "==> Editor exited non-zero; the report below is what decides."

REPORT="${REPORT_DIR}/index.json"
if [[ ! -f "${REPORT}" ]]; then
	echo "==> No report at ${REPORT}. The editor died before running anything." >&2
	exit 1
fi

python3 - "${REPORT}" <<'PYTHON'
import json, sys

with open(sys.argv[1]) as handle:
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
