#!/usr/bin/env bash
set -euo pipefail
module_dir="$(cd "$(dirname "$0")" && pwd)"
sdk_prefix="${IISDK_NATIVE_PREFIX:-$module_dir/../build/congregation-native/prefix}"
cmake -S "$module_dir" -B "$module_dir/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sdk_prefix" -DCMAKE_PREFIX_PATH="$sdk_prefix" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_FIND_FRAMEWORK=LAST -DCMAKE_DISABLE_FIND_PACKAGE_Qt5=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -DBUILD_TESTING=ON  "$@"
cmake --build "$module_dir/build" --parallel "${BUILD_JOBS:-6}"
ctest --test-dir "$module_dir/build" --output-on-failure
cmake --install "$module_dir/build"
