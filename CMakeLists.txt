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

# KUDA-Lite build.
#
#   cmake --preset release && cmake --build --preset release && ctest --preset release
#
# Components (all ON by default; turn off what a machine does not need):
#   KUDALITE_BUILD_CONTROLLER  cl-controller            (controller Pi)
#   KUDALITE_BUILD_WORKER      cl-worker + kernels      (worker Pis)
#   KUDALITE_BUILD_TOOLS       cl-info, cl-top, cl-test-memory, cl-test-matmul
#   KUDALITE_BUILD_TESTS       unit + integration tests (CTest)
#   KUDALITE_BUILD_EXAMPLES    examples/saxpy
# The host library libkudalite is always built. See docs/BUILD.md.
cmake_minimum_required(VERSION 3.21)

project(KUDA-Lite
  VERSION 0.2.0
  DESCRIPTION "GPU-style compute runtime for a Raspberry Pi 5 cluster"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(KUDALITE_BUILD_CONTROLLER "Build cl-controller" ON)
option(KUDALITE_BUILD_WORKER "Build cl-worker" ON)
option(KUDALITE_BUILD_TOOLS "Build cl-info, cl-top and the cluster test programs" ON)
option(KUDALITE_BUILD_TESTS "Build the test suite" ON)
option(KUDALITE_BUILD_EXAMPLES "Build the examples" ON)
option(KUDALITE_SHARED "Build libkudalite as a shared library" OFF)
option(KUDALITE_WERROR "Treat compiler warnings as errors" OFF)
set(KUDALITE_CPU "generic" CACHE STRING "CPU tuning: generic | native | pi5 (Cortex-A76)")
set_property(CACHE KUDALITE_CPU PROPERTY STRINGS generic native pi5)
set(KUDALITE_SANITIZER "" CACHE STRING "Sanitizer for debugging: '' | thread | address")
set_property(CACHE KUDALITE_SANITIZER PROPERTY STRINGS "" thread address)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ---- Shared compile settings ------------------------------------------------------------------

add_library(kudalite_flags INTERFACE)
target_compile_options(kudalite_flags INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
  $<$<AND:$<BOOL:${KUDALITE_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>)

if(KUDALITE_CPU STREQUAL "native")
  target_compile_options(kudalite_flags INTERFACE -mcpu=native)
elseif(KUDALITE_CPU STREQUAL "pi5")
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    message(FATAL_ERROR "KUDALITE_CPU=pi5 needs an AArch64 target (building for ${CMAKE_SYSTEM_PROCESSOR}). "
                        "Build on the Pi, or cross-compile with cmake/toolchains/aarch64-linux-gnu.cmake.")
  endif()
  target_compile_options(kudalite_flags INTERFACE -mcpu=cortex-a76)
elseif(NOT KUDALITE_CPU STREQUAL "generic")
  message(FATAL_ERROR "KUDALITE_CPU must be generic, native or pi5 (got '${KUDALITE_CPU}')")
endif()

if(KUDALITE_SANITIZER)
  if(NOT KUDALITE_SANITIZER MATCHES "^(thread|address)$")
    message(FATAL_ERROR "KUDALITE_SANITIZER must be thread or address")
  endif()
  target_compile_options(kudalite_flags INTERFACE -fsanitize=${KUDALITE_SANITIZER} -fno-omit-frame-pointer)
  target_link_options(kudalite_flags INTERFACE -fsanitize=${KUDALITE_SANITIZER})
endif()

# ---- Code shared by every program -------------------------------------------------------------

add_library(kudalite_common OBJECT
  src/common/address_map.cpp
  src/common/dsm_client.cpp
  src/common/errors.cpp
  src/common/freelist.cpp
  src/common/log.cpp
  src/common/net.cpp
  src/common/protocol.cpp
  src/common/rpc.cpp
  src/common/telemetry.cpp
  src/common/thread_pool.cpp)
target_include_directories(kudalite_common PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)
target_link_libraries(kudalite_common PUBLIC Threads::Threads $<BUILD_INTERFACE:kudalite_flags>)

# ---- Host runtime: libkudalite -----------------------------------------------------------------

if(KUDALITE_SHARED)
  set(_kudalite_kind SHARED)
else()
  set(_kudalite_kind STATIC)
endif()
add_library(kudalite ${_kudalite_kind}
  src/host/runtime.cpp
  src/host/clblas.cpp
  $<TARGET_OBJECTS:kudalite_common>)
add_library(kudalite::kudalite ALIAS kudalite)
target_include_directories(kudalite PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(kudalite PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(kudalite PUBLIC Threads::Threads PRIVATE $<BUILD_INTERFACE:kudalite_flags>)
set_target_properties(kudalite PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

install(TARGETS kudalite EXPORT kudaliteTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/kudalite DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# find_package(kudalite) support for host applications.
install(EXPORT kudaliteTargets NAMESPACE kudalite:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kudalite)
configure_package_config_file(cmake/kudaliteConfig.cmake.in ${PROJECT_BINARY_DIR}/kudaliteConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kudalite)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/kudaliteConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${PROJECT_BINARY_DIR}/kudaliteConfig.cmake ${PROJECT_BINARY_DIR}/kudaliteConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kudalite)

# ---- Daemons ----------------------------------------------------------------------------------

if(KUDALITE_BUILD_CONTROLLER)
  add_executable(cl-controller
    src/controller/main.cpp
    src/controller/controller.cpp)
  target_link_libraries(cl-controller PRIVATE kudalite_common kudalite_flags)
  install(TARGETS cl-controller RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(KUDALITE_BUILD_WORKER)
  # Kernels are compiled into the executable (not a static library) so their static
  # registration objects are never dropped by the linker.
  add_executable(cl-worker
    src/worker/main.cpp
    src/worker/worker.cpp
    src/worker/arena.cpp
    src/worker/page_cache.cpp
    src/worker/kernel_context.cpp
    src/worker/kernels/blas_kernels.cpp)
  target_link_libraries(cl-worker PRIVATE kudalite_common kudalite_flags)
  install(TARGETS cl-worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---- Tools ------------------------------------------------------------------------------------

if(KUDALITE_BUILD_TOOLS)
  add_executable(cl-info tools/cl_info.cpp)
  add_executable(cl-top tools/cl_top.cpp)
  # The cluster test programs double as post-deployment acceptance tests.
  add_executable(cl-test-memory tests/test_memory.cpp)
  add_executable(cl-test-matmul tests/test_matmul.cpp)
  foreach(t cl-info cl-top cl-test-memory cl-test-matmul)
    target_link_libraries(${t} PRIVATE kudalite kudalite_flags)
  endforeach()
  install(TARGETS cl-info cl-top cl-test-memory cl-test-matmul RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(KUDALITE_BUILD_EXAMPLES)
  add_executable(saxpy examples/saxpy.cpp)
  target_link_libraries(saxpy PRIVATE kudalite kudalite_flags)
  install(FILES examples/saxpy.cpp DESTINATION ${CMAKE_INSTALL_DATADIR}/kudalite/examples)
endif()

# ---- Deployment files (config templates + systemd units, with this prefix baked in) ----------

set(KUDALITE_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
set(KUDALITE_DOCDIR "${CMAKE_INSTALL_FULL_DATADIR}/doc/kudalite")
foreach(unit kudalite-controller kudalite-worker)
  configure_file(deploy/systemd/${unit}.service.in ${PROJECT_BINARY_DIR}/systemd/${unit}.service @ONLY)
  install(FILES ${PROJECT_BINARY_DIR}/systemd/${unit}.service DESTINATION ${CMAKE_INSTALL_DATADIR}/kudalite/systemd)
endforeach()
install(FILES deploy/config/controller.conf deploy/config/worker.conf deploy/config/dashboard.env
  DESTINATION ${CMAKE_INSTALL_DATADIR}/kudalite/config)
install(FILES README.md LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/kudalite)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/kudalite FILES_MATCHING PATTERN "*.md")

# ---- Tests ------------------------------------------------------------------------------------

if(KUDALITE_BUILD_TESTS)
  enable_testing()
  add_executable(test_unit tests/test_unit.cpp)
  target_link_libraries(test_unit PRIVATE kudalite_common kudalite_flags)
  add_test(NAME unit COMMAND test_unit)
  set_tests_properties(unit PROPERTIES LABELS unit TIMEOUT 60)

  if(KUDALITE_BUILD_CONTROLLER AND KUDALITE_BUILD_WORKER AND KUDALITE_BUILD_TOOLS AND KUDALITE_BUILD_EXAMPLES)
    find_program(BASH bash)
    if(BASH)
      # Starts a controller + 3 workers on localhost, runs the cluster tests, stops them.
      add_test(NAME integration
        COMMAND ${BASH} ${PROJECT_SOURCE_DIR}/scripts/integration-test.sh $<TARGET_FILE_DIR:cl-controller>)
      set_tests_properties(integration PROPERTIES LABELS integration TIMEOUT 900 RUN_SERIAL TRUE)
    endif()
  endif()
endif()

message(STATUS "KUDA-Lite ${PROJECT_VERSION}: ${CMAKE_BUILD_TYPE}, CPU tuning '${KUDALITE_CPU}', "
               "controller=${KUDALITE_BUILD_CONTROLLER} worker=${KUDALITE_BUILD_WORKER} "
               "tools=${KUDALITE_BUILD_TOOLS} tests=${KUDALITE_BUILD_TESTS}")
