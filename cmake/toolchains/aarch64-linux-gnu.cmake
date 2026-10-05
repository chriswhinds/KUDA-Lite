# Copyright 2026 Christopher Hinds, Stratum Labs
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Cross-compile KUDA-Lite for Raspberry Pi 5 (AArch64 Linux) from an x86-64 Linux machine.
#
#   Debian/Ubuntu:  sudo apt install g++-aarch64-linux-gnu
#   cmake --preset pi5-cross && cmake --build --preset pi5-cross
#
# The binaries link against the cross toolchain's glibc and libstdc++, so the Pis must run a
# distribution at least as new as the toolchain (or link statically: add
# -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"). Building natively on a Pi
# (deploy/install.sh does this) avoids the question entirely.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_PREFIX "aarch64-linux-gnu-" CACHE STRING "Cross toolchain prefix")
set(CMAKE_C_COMPILER ${CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_PREFIX}g++)

# Optional sysroot copied from a Pi (rsync -a pi:/usr/lib pi:/usr/include ...).
if(DEFINED ENV{PI_SYSROOT})
  set(CMAKE_SYSROOT $ENV{PI_SYSROOT})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
