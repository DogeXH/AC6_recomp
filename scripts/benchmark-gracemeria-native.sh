#!/usr/bin/env bash
set -euo pipefail

source_root="${AC6_SOURCE_ROOT:-/home/deck/Projects/AC6-Recomp-Linux-src}"
trace_path="${AC6_GRACEMERIA_TRACE:-/home/deck/Projects/ac6-gracemeria-runtime-state-20260821/4E4D07D1_12804.xtr}"
result_root="${AC6_GRACEMERIA_RESULTS:-/home/deck/Projects/ac6-gracemeria-runtime-state-20260821/results}"
expected_trace_sha="1ca6f97a2bc913ad370cb6317d39165b635436ea65eff7fcc8309e205b6a3b62"
expected_resolve_signature_sha="e4718396abc548305923ea0c813240fa97e576c9d6b5339012ab94647de7384f"
expected_final_ppm_sha="a31a3c170f5437044531e2c04687d75462febd974c208d13b794c19f44e6c388"
label="${1:-candidate}"

match_lines() {
  if command -v rg >/dev/null 2>&1; then
    rg "$1"
  else
    grep -E "$1"
  fi
}

if [[ ! "$label" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "Benchmark label must contain only letters, digits, dot, underscore, or dash." >&2
  exit 2
fi

tool="$source_root/out/build/linux-amd64-relwithdebinfo/ac6-nr2-trace-draw"
if [[ ! -x "$tool" ]]; then
  echo "Missing trace benchmark binary: $tool" >&2
  exit 3
fi
if [[ ! -f "$trace_path" ]]; then
  echo "Missing Gracemeria trace: $trace_path" >&2
  exit 4
fi

actual_trace_sha="$(sha256sum "$trace_path" | awk '{print $1}')"
if [[ "$actual_trace_sha" != "$expected_trace_sha" ]]; then
  echo "Trace checksum mismatch: expected $expected_trace_sha, got $actual_trace_sha" >&2
  exit 5
fi

mkdir -p "$result_root"
log_path="$result_root/$label.log"
timing_path="$result_root/$label.time.txt"
signature_path="$result_root/$label.resolve-signature.txt"
normalized_log_path="$result_root/$label.normalized.log"
output_path="$result_root/$label.final.ppm"
resolve_dump_dir="$result_root/$label.resolve-dumps"
final_native_resolve="$resolve_dump_dir/resolve-1636-native.ppm"

if [[ -e "$resolve_dump_dir" ]]; then
  echo "Resolve dump directory already exists; choose a fresh benchmark label: $resolve_dump_dir" >&2
  exit 2
fi
mkdir -p "$resolve_dump_dir"

/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%M' -o "$timing_path" \
  env AC6_NR2_DEFER_DRAW_WAITS=1 AC6_NR2_GPU_RESOLVE_SHADOW=1 \
  AC6_NR2_FORCE_GUEST_MEMORY_COPY=1 \
  AC6_NR2_DUMP_RESOLVE_DIR="$resolve_dump_dir" \
  AC6_NR2_DUMP_RESOLVE_MIN_CAPTURE=1636 AC6_NR2_DUMP_RESOLVE_ONCE=1 \
  "$tool" range "$trace_path" 0 1637 "$output_path" \
  > "$log_path" 2>&1

tr '\000' '\n' < "$log_path" > "$normalized_log_path"
match_lines 'nr2: native resolve capture|draw snapshot #[0-9]+ rejected' \
  < "$normalized_log_path" \
  | sed -E 's/capture #[0-9]+/capture #N/g; s/snapshot #[0-9]+/snapshot #N/g' \
  > "$signature_path"

actual_resolve_signature_sha="$(sha256sum "$signature_path" | awk '{print $1}')"
if [[ "$actual_resolve_signature_sha" != "$expected_resolve_signature_sha" ]]; then
  echo "Resolve signature mismatch: expected $expected_resolve_signature_sha, got $actual_resolve_signature_sha" >&2
  exit 6
fi
if match_lines 'GPU resolve shadow #[0-9]+ FAILED|gpu_resolve_shadow_mismatch|entered fatal state' \
    < "$normalized_log_path" >/dev/null; then
  echo "GPU resolve shadow validation failed; inspect $log_path" >&2
  exit 7
fi
shadow_passes="$(
  (match_lines 'GPU resolve shadow #[0-9]+ passed' \
    < "$normalized_log_path" || true) | wc -l | tr -d ' '
)"
if [[ "$shadow_passes" -lt 1 ]]; then
  echo "GPU resolve shadow path was not exercised; inspect $log_path" >&2
  exit 8
fi
if [[ ! -s "$final_native_resolve" ]]; then
  echo "Missing final native resolve output: $final_native_resolve" >&2
  exit 9
fi
cp "$final_native_resolve" "$output_path"

actual_ppm_sha="$(sha256sum "$output_path" | awk '{print $1}')"
if [[ "$actual_ppm_sha" != "$expected_final_ppm_sha" ]]; then
  echo "Final PPM checksum mismatch: expected $expected_final_ppm_sha, got $actual_ppm_sha" >&2
  exit 10
fi

echo "trace_sha256=$actual_trace_sha"
echo "resolve_signature_sha256=$actual_resolve_signature_sha"
echo "gpu_resolve_shadow_passes=$shadow_passes"
echo "final_ppm_sha256=$actual_ppm_sha"
cat "$timing_path"
match_lines 'guest texture allocation hashes|Native range|nr2-benchmark-phases' \
  < "$normalized_log_path" \
  | tail -3
