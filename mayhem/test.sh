#!/usr/bin/env bash
#
# mayhem/test.sh — run the known-answer tests built by mayhem/build.sh (no compiling here).
# Each selftest drives the real Modbus slave/master task loops with fixed request/response frames and
# compares the transmitted bytes, register images and completion codes against spec values.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

passed=0 failed=0
for variant in rtu tcp; do
  runner="/mayhem/selftest_$variant"
  if [ ! -x "$runner" ]; then
    echo "missing $runner — mayhem/build.sh should have built it" >&2
    failed=$((failed + 1))
    continue
  fi
  out="$(timeout 120 "$runner" 2>&1)"
  rc=$?
  printf '%s\n' "$out"
  p="$(printf '%s\n' "$out" | grep -c '^PASS ')"
  f="$(printf '%s\n' "$out" | grep -c '^FAIL ')"
  passed=$((passed + p))
  failed=$((failed + f))
  # a crash/timeout/early exit that printed no FAIL line still counts as a failure
  if ! printf '%s\n' "$out" | grep -q '^selftest: [0-9]* passed, [0-9]* failed$' || { [ "$rc" -ne 0 ] && [ "$f" -eq 0 ]; }; then
    failed=$((failed + 1))
  fi
done

emit_ctrf "modbus-selftest" "$passed" "$failed" 0
