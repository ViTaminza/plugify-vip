#!/bin/bash
# Compile check of the vip plugin sources inside the plugify-builder image, without CMake.
# Used while another plugin's CMakeLists breaks the repository-wide configure step.
# Usage: bash plugins/vip/check.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
exec 8>"$ROOT/.build.lock"; flock 8
docker run --rm -v "$ROOT":/src -w /src --entrypoint bash plugify-builder:1 -c '
set -e
mkdir -p /tmp/vipcheck
CXX="g++-14 -std=c++23 -Wall -Wextra -Werror -fPIC -D_GLIBCXX_USE_CXX11_ABI=1"
INC="-I/src/sdk/include -I/src/libs/pisexcompat/include -I/src/libs/plgdb/include -I/src/plugins/vip/src -I/src/plugins/vip/include/external/plugify/include"
$CXX $INC -c /src/plugins/vip/src/vip_core.cpp   -o /tmp/vipcheck/vip_core.o
$CXX $INC -c /src/plugins/vip/src/plugin.cpp     -o /tmp/vipcheck/plugin.o
$CXX $INC -c /src/plugins/vip/src/commands.cpp   -o /tmp/vipcheck/commands.o
$CXX $INC -c /src/plugins/vip/src/menus.cpp      -o /tmp/vipcheck/menus.o
$CXX $INC -I/src/plugins/vip/include -c /src/plugins/vip/tests/vip_core_tests.cpp -o /tmp/vipcheck/vip_core_tests.o
$CXX $INC -I/src/plugins/vip/include -c /src/plugins/vip/tests/vip_api_tests.cpp  -o /tmp/vipcheck/vip_api_tests.o
echo VIP_CHECK_OK
'
