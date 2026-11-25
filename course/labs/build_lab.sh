#!/usr/bin/env bash
# Builds one C++ lab of the course against the libraries of the release build.
#
#   course/labs/build_lab.sh course/labs/lab02_book_repl.cpp
#
# The program is written to build/release/labs/<name>. Build lobsim first:
#   cmake --preset release && cmake --build --preset release -j
# Set LOBSIM_BUILD to use another build folder, and CXX to use another compiler.
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "usage: $0 <lab file .cpp>" >&2
  exit 2
fi

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="${LOBSIM_BUILD:-$root/build/release}"
src="$1"
name="$(basename "$src" .cpp)"

for lib in libsim.a liborderlog.a libcalibfile.a liblob.a; do
  if [ ! -f "$build/$lib" ]; then
    echo "missing $build/$lib: build lobsim first with" >&2
    echo "  cmake --preset release && cmake --build --preset release -j" >&2
    exit 1
  fi
done

mkdir -p "$build/labs"
# -ffp-contract=off is the same flag the simulator library is built with (DECISIONS.md D-078).
# The order of the libraries matters: a library comes before the libraries it uses.
"${CXX:-c++}" -std=c++20 -O2 -ffp-contract=off -Wall -Wextra \
  -I"$root/include" -I"$build/_deps/nlohmann_json-src/include" \
  "$src" \
  "$build/libsim.a" "$build/liborderlog.a" "$build/libcalibfile.a" "$build/liblob.a" \
  -o "$build/labs/$name"
echo "built $build/labs/$name"
