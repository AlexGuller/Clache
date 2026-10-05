#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
source "${MATRIX_CONFIG:-$ROOT/benchmarks/configs/matrix.env}"
: "${BIN_DIR:=$ROOT/build/release/apps}"
: "${TRACE_DIR:=$ROOT/benchmarks/traces}"
: "${RESULT_DIR:=$ROOT/benchmarks/results/$(date -u +%Y%m%dT%H%M%SZ)}"
: "${JOBS:=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
: "${DRY_RUN:=0}"
: "${DOMAIN_BY:=none}"
export WORKLOADS SEEDS REQUESTS KEYS VARIABLE_SIZE_KEYS CAPACITY_FRACTIONS WARMUP_FRACTION POLICIES BIN_DIR TRACE_DIR RESULT_DIR JOBS DOMAIN_BY
run() {
  if [[ "$DRY_RUN" == 1 ]]; then printf '%q ' "$@"; printf '\n'; else "$@"; fi
}
if [[ "$DRY_RUN" != 1 ]]; then
  for binary in clache_tracegen clache_sim; do
    if [[ ! -x "$BIN_DIR/$binary" ]]; then echo "error: missing executable $BIN_DIR/$binary; build Release binaries or set BIN_DIR" >&2; exit 2; fi
  done
  command -v python3 >/dev/null || { echo 'error: python3 is required' >&2; exit 2; }
  mkdir -p "$TRACE_DIR" "$RESULT_DIR"
  python3 - "$ROOT" "$RESULT_DIR/env.json" <<'PY'
import json, os, platform, subprocess, sys
from pathlib import Path
root, output = map(Path, sys.argv[1:])
def capture(command):
    try: return subprocess.check_output(command, cwd=root, text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError): return 'unavailable'
cache = root / 'build/release/CMakeCache.txt'
compiler = {}
if cache.exists():
    for line in cache.read_text().splitlines():
        if line.startswith(('CMAKE_CXX_COMPILER:', 'CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELEASE:')):
            key, value = line.split('=', 1); compiler[key.split(':')[0]] = value
if 'CMAKE_CXX_COMPILER' in compiler:
    compiler['version'] = capture([compiler['CMAKE_CXX_COMPILER'], '--version'])
cpu = platform.processor()
if sys.platform == 'darwin': cpu = capture(['sysctl', '-n', 'machdep.cpu.brand_string'])
elif Path('/proc/cpuinfo').exists():
    cpu = next((s.split(':', 1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')), cpu)
parameters = {k: os.environ[k] for k in 'WORKLOADS SEEDS REQUESTS KEYS VARIABLE_SIZE_KEYS CAPACITY_FRACTIONS WARMUP_FRACTION POLICIES BIN_DIR TRACE_DIR RESULT_DIR JOBS DOMAIN_BY'.split()}
output.write_text(json.dumps(dict(cpu=cpu, os=platform.platform(), git_sha=capture(['git', 'rev-parse', 'HEAD']), compiler=compiler, parameters=parameters), sort_keys=True, indent=2)+'\n')
PY
else
  printf 'DRY RUN: env.json would be written to %q\n' "$RESULT_DIR/env.json"
fi
failed=0
results=()
for workload in $WORKLOADS; do
  for seed in $SEEDS; do
    trace="$TRACE_DIR/$workload-seed$seed.ctrace"
    output="$RESULT_DIR/$workload-seed$seed.csv"
    keys=$KEYS
    if [[ "$workload" == variable_size ]]; then keys=$VARIABLE_SIZE_KEYS; fi
    if [[ ! -e "$trace" && ! -L "$trace" ]]; then
      if [[ -e "$trace.meta.json" || -L "$trace.meta.json" ]]; then
        echo "error: refusing to overwrite orphan sidecar $trace.meta.json; use a different TRACE_DIR or explicitly replace the stale cache" >&2
        failed=1; continue
      fi
      if ! run "$BIN_DIR/clache_tracegen" --workload "$workload" --seed "$seed" --requests "$REQUESTS" --keys "$keys" --format bin --out "$trace"; then
        echo "error: trace generation failed: $workload seed $seed" >&2
        # The writer may have left an incomplete file; never reuse it as a trace.
        if [[ -f "$trace" ]]; then mv "$trace" "$trace.failed"; fi
        failed=1; continue
      fi
    fi
    if ! run python3 "$SCRIPT_DIR/validate_trace_cache.py" "$trace" "$workload" "$seed" "$REQUESTS" "$keys"; then
      failed=1; continue
    fi
    if run "$BIN_DIR/clache_sim" --trace "$trace" --policies "$POLICIES" --capacity-fractions "$CAPACITY_FRACTIONS" --warmup-fraction "$WARMUP_FRACTION" --domain-by "$DOMAIN_BY" --jobs "$JOBS" --out "$output"; then
      results+=("$output")
    else
      echo "error: simulation failed: $workload seed $seed" >&2
      failed=1
    fi
  done
done
if [[ ${#results[@]} -gt 0 ]]; then
  if ! run python3 "$SCRIPT_DIR/report.py" "${results[@]}" --out "$RESULT_DIR/report"; then failed=1; fi
else
  echo 'error: no successful experiments to report' >&2
  failed=1
fi
exit "$failed"
