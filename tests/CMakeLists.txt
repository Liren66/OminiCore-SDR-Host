cmake_minimum_required(VERSION 3.20)
project(OmniCoreHostTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
enable_testing()
set(OCH_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Offline CLI tests: parsing, FTR files and terminal presentation.
# Header-only, no device or Windows API required.
add_executable(och_cli_tests cli_tests.cpp)
target_include_directories(och_cli_tests PRIVATE ${OCH_ROOT}/include ${OCH_ROOT}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(och_cli_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(MINGW)
  target_link_options(och_cli_tests PRIVATE -static)
endif()
add_test(NAME cli_offline COMMAND och_cli_tests)
set_tests_properties(cli_offline PROPERTIES TIMEOUT 30)

# Runtime tests compile the production C API, configuration transaction,
# stream worker, codecs and RF setters against an in-process fake USB
# transport. They use Windows events and threads.
if(WIN32)
  add_executable(och_runtime_tests
    runtime_tests.cpp fake_device.cpp
    ${OCH_ROOT}/src/core/api.cpp
    ${OCH_ROOT}/src/core/configuration.cpp
    ${OCH_ROOT}/src/core/reconfigure.cpp
    ${OCH_ROOT}/src/core/stream.cpp
    ${OCH_ROOT}/src/core/rf.cpp
    ${OCH_ROOT}/src/core/board.cpp
    ${OCH_ROOT}/src/core/power.cpp
    ${OCH_ROOT}/src/dsp/codec.cpp
    ${OCH_ROOT}/src/dsp/framer.cpp)
  target_include_directories(och_runtime_tests PRIVATE ${OCH_ROOT}/include ${OCH_ROOT}/src)
  target_compile_definitions(och_runtime_tests PRIVATE OCH_BUILD UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(och_runtime_tests PRIVATE Threads::Threads setupapi ole32 advapi32)
  if(MINGW)
    target_compile_options(och_runtime_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(och_runtime_tests PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  add_test(NAME runtime_reconfiguration COMMAND och_runtime_tests)
  set_tests_properties(runtime_reconfiguration PROPERTIES TIMEOUT 30)
else()
  message(STATUS "Runtime tests need Windows; building the offline CLI tests only")
endif()
