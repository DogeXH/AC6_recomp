#!/bin/bash
# Starts one test instance created by make_instance.sh (its launch.sh calls this).
#
#   launch_instance.sh <instance-dir> [--input-script FILE] [--max-rss-mib N] [--dry-run]
#                      [-- extra game flags]
#
# Runs the game binary with --user_data_root=<dir> and --game_data_root=<assets>, with the cwd
# set to <dir> (boot.log and ac6recomp.log are written to the cwd), console output in
# <dir>/console.log and the PID in <dir>/ac6recomp.pid. GPU flags and MoltenVK settings match
# Cole's guarded launcher (run-ac6-guarded.sh, native720 profile). The previous run's trace,
# console and app logs move to <dir>/history/. rss_guard.sh stops the game if its resident
# memory passes --max-rss-mib (default 10240, the guarded launcher's cutoff; 0 = off).
set -euo pipefail

# shellcheck source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
  sed -n '2,13p' "$0" >&2
  exit 2
}

[ "$#" -ge 1 ] || usage
dir="$1"
shift
input_script=""
dry_run=0
max_rss_mib=10240
extra=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --input-script)
      [ "$#" -ge 2 ] || die "--input-script needs a file"
      input_script="$2"
      shift
      ;;
    --max-rss-mib)
      [ "$#" -ge 2 ] || die "--max-rss-mib needs a number"
      case "$2" in "" | *[!0-9]*) die "--max-rss-mib needs a number" ;; esac
      max_rss_mib="$2"
      shift
      ;;
    --dry-run) dry_run=1 ;;
    --)
      shift
      extra=("$@")
      break
      ;;
    -h | --help) usage ;;
    *) die "unknown option '$1'" ;;
  esac
  shift
done

[ -f "$dir/ac6recomp.toml" ] || die "$dir is not an instance (no ac6recomp.toml); run make_instance.sh"
dir="$(cd "$dir" && pwd -P)"
name="${dir##*/}"
pidfile="$dir/ac6recomp.pid"
if pid="$(pid_from_file "$pidfile" ac6recomp)"; then
  die "instance $name is already running (PID $pid)"
fi

if [ -n "$input_script" ]; then
  [ -f "$input_script" ] || die "input script $input_script not found"
  input_source="$(cd "$(dirname "$input_script")" && pwd -P)/$(basename "$input_script")"
  # Each instance runs its own copy, so its progress file (<script>.progress) is per instance
  # and the exact script stays with the run's evidence.
  input_script="$dir/input_script.txt"
fi

cmd=(
  "$AC6NP_BINARY"
  "--user_data_root=$dir"
  "--game_data_root=$AC6NP_ASSETS"
  "--resolution=720p"
  "--video_mode_width=1280"
  "--video_mode_height=720"
  "--high_pixel_density=false"
  "--vulkan_persistent_pipeline_cache=1"
  "--render_target_path_vulkan=fsi"
  "--vulkan_vertex_buffer_residency_cache=1"
  "--primitive_processor_cache_min_indices=4096"
  "--log_max_file_size_mb=4"
  "--log_max_files=2"
  "--log_flush_interval=1"
)
[ -z "$input_script" ] || cmd+=("--netplay_input_script=$input_script")
[ "${#extra[@]}" -eq 0 ] || cmd+=("${extra[@]}")

# MoltenVK settings exported by run-ac6-guarded.sh (synchronous submits, bounded command buffers).
mvk_env=(
  MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1
  MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE=16
  MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0
  MVK_CONFIG_FAST_MATH_ENABLED=1
  MVK_CONFIG_LOG_LEVEL=1
)

if [ "$dry_run" -eq 1 ]; then
  note "dry run for instance $name (cwd $dir, console $dir/console.log)"
  [ -x "$AC6NP_BINARY" ] || note "warning: binary not found: $AC6NP_BINARY"
  [ -f "$AC6NP_ASSETS/default.xex" ] || note "warning: no default.xex in $AC6NP_ASSETS"
  printf 'env'
  printf ' %q' "${mvk_env[@]}"
  printf ' \\\n '
  printf ' %q' "${cmd[@]}"
  printf '\n'
  exit 0
fi

[ -x "$AC6NP_BINARY" ] || die "game binary not found: $AC6NP_BINARY (set AC6NP_BINARY)"
[ -f "$AC6NP_ASSETS/default.xex" ] || die "no default.xex in $AC6NP_ASSETS (set AC6NP_ASSETS)"

# Keep each run's evidence separate: move the previous run's files aside (never deleted).
stamp="$(timestamp)"
[ -z "$input_script" ] || cp "$input_source" "$input_script.new"
for f in trace.jsonl console.log ac6recomp.log input_script.txt input_script.txt.progress; do
  if [ -s "$dir/$f" ]; then
    mkdir -p "$dir/history"
    mv -f "$dir/$f" "$dir/history/${f%.*}-$stamp.${f##*.}"
  fi
done
[ -z "$input_script" ] || mv -f "$input_script.new" "$input_script"

# Debug dumps that run-ac6-guarded.sh also refuses to inherit.
unset AC6_AUDIO_DUMP AC6_DUMP_PAC_DECODED AC6_TRACE_PAC_WORK_ITEMS AC6_TRACE_PAC_STACKS \
  AC6_TRACE_CODEC_INTERNALS

cd "$dir"
env "${mvk_env[@]}" nohup "${cmd[@]}" < /dev/null > "$dir/console.log" 2>&1 &
pid=$!
printf '%s\n' "$pid" > "$pidfile"
{
  printf 'started %s pid %s\n' "$(date '+%Y-%m-%d %H:%M:%S %z')" "$pid"
  printf 'command:'
  printf ' %q' "${cmd[@]}"
  printf '\n'
} > "$dir/launch.last"

# env and nohup exec the game, so $! is the game itself; give it a moment to fail fast.
sleep 1
if ! pid_from_file "$pidfile" ac6recomp > /dev/null; then
  sleep 1
  if ! pid_from_file "$pidfile" ac6recomp > /dev/null; then
    tail -n 20 "$dir/console.log" | sed -e 's/^/  console: /' >&2 || true
    rm -f "$pidfile"
    die "instance $name exited during startup (see $dir/console.log)"
  fi
fi

if [ "$max_rss_mib" -gt 0 ]; then
  nohup "$AC6NP_TOOLS_DIR/rss_guard.sh" "$pidfile" "$max_rss_mib" "$dir/rss_guard.log" \
    < /dev/null > /dev/null 2>&1 &
  printf '%s\n' "$!" > "$dir/rss_guard.pid"
fi

note "instance $name started: PID $pid (pidfile $pidfile)"
note "  console $dir/console.log, trace $dir/trace.jsonl"
[ -z "$input_script" ] || note "  input script $input_source -> $input_script (progress: $input_script.progress)"
