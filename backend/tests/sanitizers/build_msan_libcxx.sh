#!/usr/bin/env bash
# Builds libc++ and libc++abi instrumented for MemorySanitizer (with origins), from the LLVM
# release that matches the given clang, and installs them into <prefix>. The msan preset links
# against them: MemorySanitizer reports every read of memory that an uninstrumented library
# wrote, so the C++ standard library must be instrumented too.
#
# usage: build_msan_libcxx.sh <clang++> <prefix> [<work directory>]
#
# The work directory (default <prefix>.work) keeps the LLVM sources and the build; it can be
# removed afterwards. Output: one `msan_libcxx=<prefix> llvm=<version>` line; exit 0 on success.
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: build_msan_libcxx.sh <clang++> <prefix> [<work directory>]" >&2
  exit 2
fi
cxx=$1
prefix=$2
work=${3:-$prefix.work}
cc=${cxx/clang++/clang}

version=$("$cxx" -dumpversion)
case "$version" in
  *.*.*) ;;
  *) echo "msan_libcxx=failed reason=clang-version detail=$version" >&2; exit 1 ;;
esac

mkdir -p "$work"
src=$work/llvm-project-$version
if [ ! -d "$src/runtimes" ]; then
  rm -rf "$src"
  git clone --quiet --depth 1 --branch "llvmorg-$version" --filter=blob:none --sparse \
    https://github.com/llvm/llvm-project "$src"
fi
# libc: libc++'s floating-point std::from_chars is built from LLVM libc's shared sources.
git -C "$src" sparse-checkout set runtimes libcxx libcxxabi libunwind libc cmake llvm/cmake \
  llvm/utils/llvm-lit third-party

cmake -G Ninja -S "$src/runtimes" -B "$work/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
  -DLLVM_USE_SANITIZER=MemoryWithOrigins \
  -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_ENABLE_STATIC=OFF \
  -DLIBCXXABI_ENABLE_SHARED=ON -DLIBCXXABI_ENABLE_STATIC=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF
cmake --build "$work/build" -j "${JOBS:-8}"
cmake --install "$work/build" >/dev/null
test -f "$prefix/include/c++/v1/vector"
test -e "$prefix/lib/libc++.so"
echo "msan_libcxx=$prefix llvm=$version"
