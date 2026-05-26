#!/usr/bin/env bash

# Run the SQLite overlap sweep using the benchmark executable beside this script.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
benchmark="${script_dir}/filter_build_sqlite3"

build_rows=${BUILD_ROWS:-33554432}
probe_rows=${PROBE_ROWS:-1048576}
trials=${TRIALS:-10}
warmup=${WARMUP:-true}
validate=${VALIDATE:-false}
cpu_core=${CPU_CORE:-}
overlaps=${OVERLAPS:-"1 5 10 20 30"}
run_id=$(date +%Y%m%d_%H%M%S)
output_dir=${OUTPUT_DIR:-"${script_dir}/overlap_sweep_${run_id}"}
database=${DATABASE:-"${TMPDIR:-/tmp}/filter_build_overlap_sweep_${run_id}.sqlite"}

case "${validate}" in
  true)
    validation_args=(--validate)
    ;;
  false)
    validation_args=()
    ;;
  *)
    printf 'error: VALIDATE must be true or false: %s\n' "${validate}" >&2
    exit 1
    ;;
esac

if [[ ! -x "${benchmark}" ]]; then
  printf 'error: benchmark executable not found: %s\n' "${benchmark}" >&2
  exit 1
fi

benchmark_command=("${benchmark}")
if [[ -n "${cpu_core}" ]]; then
  if [[ ! "${cpu_core}" =~ ^[0-9]+$ ]]; then
    printf 'error: CPU_CORE must be a non-negative integer: %s\n' "${cpu_core}" >&2
    exit 1
  fi
  if ! command -v taskset >/dev/null 2>&1; then
    printf 'error: CPU_CORE requires taskset\n' >&2
    exit 1
  fi
  if ! taskset --cpu-list "${cpu_core}" true >/dev/null 2>&1; then
    printf 'error: CPU_CORE is offline or outside the allowed CPU set: %s\n' \
      "${cpu_core}" >&2
    exit 1
  fi
  benchmark_command=(taskset --cpu-list "${cpu_core}" "${benchmark}")
fi

mkdir -p -- "${output_dir}"

printf 'Filter-build overlap sweep\n'
printf '  overlaps: %s\n' "${overlaps}"
printf '  build rows: %s\n' "${build_rows}"
printf '  probe rows: %s\n' "${probe_rows}"
printf '  trials: %s\n' "${trials}"
printf '  warmup: %s\n' "${warmup}"
printf '  validate: %s\n' "${validate}"
printf '  CPU core: %s\n' "${cpu_core:-unbound}"
printf '  database: %s\n' "${database}"
printf '  output directory: %s\n' "${output_dir}"

loaded=false
for overlap in ${overlaps}; do
  if [[ ! "${overlap}" =~ ^[0-9]+$ ]] || ((overlap > 100)); then
    printf 'error: invalid overlap percentage: %s\n' "${overlap}" >&2
    exit 1
  fi

  overlap_dir=$(printf '%s/overlap_%02d' "${output_dir}" "${overlap}")
  mkdir -p -- "${overlap_dir}"

  if [[ "${loaded}" == false ]]; then
    load_action=--load
    loaded=true
  else
    load_action=--reload_probes
  fi

  printf '\n=== overlap %s%%: loading probes ===\n' "${overlap}"
  "${benchmark_command[@]}" \
    "${load_action}" \
    --database="${database}" \
    --build_rows="${build_rows}" \
    --probe_rows="${probe_rows}" \
    --overlap_percent="${overlap}" \
    | tee "${overlap_dir}/load.log"

  printf '\n=== overlap %s%%: running all filters ===\n' "${overlap}"
  result_file="${overlap_dir}/results.csv"
  "${benchmark_command[@]}" \
    --run \
    --database="${database}" \
    --trials="${trials}" \
    --warmup="${warmup}" \
    "${validation_args[@]}" \
    | tee "${result_file}"
done

printf '\nSweep complete. Results: %s\n' "${output_dir}"
printf 'Database retained at: %s\n' "${database}"
