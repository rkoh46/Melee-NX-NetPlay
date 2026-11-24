#!/usr/bin/env bash
# Checks the frontend's inference engine (frontend/src/proai/onnx_lite.cpp) against onnxruntime for every Phillip policy
# in a folder: proai_record.py runs each policy through onnxruntime for N steps (synthetic game states, the policy's own
# outputs fed back as history) and writes inputs and outputs; proai_check runs the same inputs through onnx_lite and
# compares. Then, optionally, the step time of the aarch64 Linux build (the same code as the console's, NEON) in the
# mu-arm-gcc15 container.
#
#   tools/proai_check.sh MODELS_DIR [STEPS] [--docker]
#
# MODELS_DIR holds the .onnx files (any depth). Needs build/proai-venv (python3 -m venv build/proai-venv; pip install
# onnx onnxruntime numpy). Writes to build/proai-check/. The models are data: nothing in them is executed.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
models="${1:?models folder}"; steps="${2:-600}"; docker_too="${3:-}"
out="$repo/build/proai-check"; mkdir -p "$out"
py="$repo/build/proai-venv/bin/python"
[ -x "$py" ] || { echo "make build/proai-venv first (python3 -m venv build/proai-venv; pip install onnx onnxruntime numpy)" >&2; exit 1; }
src="$repo/experiments/native-engine/frontend/src/proai"
c++ -std=c++17 -O2 -Wall -I"$src" "$here/proai_check.cpp" "$src/onnx_lite.cpp" -o "$out/proai_check"
pass=0; fail=0
while IFS= read -r m; do
  name="$(basename "$m" .onnx)"
  [ -f "$out/$name.rec" ] || "$py" -I "$here/proai_record.py" "$m" "$out/$name.rec" --steps "$steps" > /dev/null
  if "$out/proai_check" "$m" "$out/$name.rec" > "$out/$name.txt" 2>&1; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
  echo "$name: $(grep -E '^RESULT' "$out/$name.txt" | cut -c9-) | $(grep -o 'step time.*' "$out/$name.txt")"
done < <(find "$models" -name '*.onnx' | sort)
echo "proai_check: $pass policies the same within tolerance, $fail different"
if [ "$docker_too" = "--docker" ]; then
  # The aarch64 Linux build (GCC 15.2, NEON) times the steps; one container, no network, the models read-only.
  mkdir -p "$out/aarch64"
  timeout 1500 docker run --rm --name mnx-proai-check --network none -v "$repo/experiments/native-engine:/src:ro" \
    -v "$(cd "$models" && pwd -P):/models:ro" -v "$out:/out" mu-arm-gcc15 bash -c '
      g++ -std=c++17 -O2 -I/src/frontend/src/proai /src/tools/proai_check.cpp /src/frontend/src/proai/onnx_lite.cpp -o /out/aarch64/proai_check &&
      for m in $(find /models -name "*.onnx" | sort); do n=$(basename "$m" .onnx); [ -f /out/$n.rec ] || continue
        for t in 1 2; do echo "$n threads $t: $(/out/aarch64/proai_check "$m" /out/$n.rec --threads $t | grep -E "^RESULT|step time" | tr "\n" " ")"; done
      done' | tee "$out/aarch64/times.txt"
fi
[ "$fail" -eq 0 ]
