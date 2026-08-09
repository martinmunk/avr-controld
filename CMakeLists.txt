cmake_minimum_required(VERSION 3.10)
project(alsa-avr-control VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${ALSA_INCLUDE_DIRS}
)

# Source files
set(BACKEND_SOURCES
    src/backends/denon_ip.cpp
    src/backends/cec.cpp
    src/backends/opalum_serial.cpp
    src/backends/nad_serial.cpp
)

set(CORE_SOURCES
    src/backend_factory.cpp
    src/config.cpp
)

# ALSA plugin libraries - Separate plugin per backend
# Denon plugin
add_library(libasound_module_ctl_denon SHARED
    src/plugins/alsa_plugin_denon.cpp
    src/backends/socket_client.cpp
    src/config.cpp
)
set_target_properties(libasound_module_ctl_denon PROPERTIES PREFIX "")
target_link_libraries(libasound_module_ctl_denon ${ALSA_LIBRARIES} Threads::Threads)
target_compile_options(libasound_module_ctl_denon PRIVATE -Wall -Wextra -Wpedantic -O2 -fPIC)
target_compile_definitions(libasound_module_ctl_denon PRIVATE PIC)

# NAD plugin
add_library(libasound_module_ctl_nad SHARED
    src/plugins/alsa_plugin_nad.cpp
    src/backends/socket_client.cpp
    src/config.cpp
)
set_target_properties(libasound_module_ctl_nad PROPERTIES PREFIX "")
target_link_libraries(libasound_module_ctl_nad ${ALSA_LIBRARIES} Threads::Threads)
target_compile_options(libasound_module_ctl_nad PRIVATE -Wall -Wextra -Wpedantic -O2 -fPIC)
target_compile_definitions(libasound_module_ctl_nad PRIVATE PIC)

# Opalum plugin
add_library(libasound_module_ctl_opalum SHARED
    src/plugins/alsa_plugin_opalum.cpp
    src/backends/socket_client.cpp
    src/config.cpp
)
set_target_properties(libasound_module_ctl_opalum PROPERTIES PREFIX "")
target_link_libraries(libasound_module_ctl_opalum ${ALSA_LIBRARIES} Threads::Threads)
target_compile_options(libasound_module_ctl_opalum PRIVATE -Wall -Wextra -Wpedantic -O2 -fPIC)
target_compile_definitions(libasound_module_ctl_opalum PRIVATE PIC)

# CEC plugin
add_library(libasound_module_ctl_cec SHARED
    src/plugins/alsa_plugin_cec.cpp
    src/backends/socket_client.cpp
    src/config.cpp
)
set_target_properties(libasound_module_ctl_cec PROPERTIES PREFIX "")
target_link_libraries(libasound_module_ctl_cec ${ALSA_LIBRARIES} Threads::Threads)
target_compile_options(libasound_module_ctl_cec PRIVATE -Wall -Wextra -Wpedantic -O2 -fPIC)
target_compile_definitions(libasound_module_ctl_cec PRIVATE PIC)

# Installation
# ALSA plugins must land in the HOST arch's multiarch alsa-lib dir, which is
# where libasound actually searches on Debian (e.g.
# /usr/lib/aarch64-linux-gnu/alsa-lib on arm64). GNUInstallDirs gives us
# CMAKE_INSTALL_LIBDIR = lib/<triplet>, and debhelper sets it correctly for the
# host arch (cross-safe). The old logic hardcoded x86_64 then fell back to
# /usr/lib/alsa-lib, so arm64 and every cross build installed to a dir ALSA does
# not search.
include(GNUInstallDirs)
if(NOT DEFINED ALSA_PLUGIN_DIR)
    set(ALSA_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/alsa-lib")
endif()

message(STATUS "ALSA plugin will be installed to: ${ALSA_PLUGIN_DIR}")

install(TARGETS
    libasound_module_ctl_denon
    libasound_module_ctl_nad
    libasound_module_ctl_opalum
    libasound_module_ctl_cec
    LIBRARY DESTINATION ${ALSA_PLUGIN_DIR}
)

# Install ALSA configuration files to /etc/alsa/conf.d/
set(ALSA_CONF_DIR "/etc/alsa/conf.d")
message(STATUS "ALSA configuration files will be installed to: ${ALSA_CONF_DIR}")

install(FILES
    examples/conf.d/50-denon-avr.conf
    examples/conf.d/50-nad-serial.conf
    examples/conf.d/50-opalum-serial.conf
    examples/conf.d/50-cec-control.conf
    DESTINATION ${ALSA_CONF_DIR}
)

# Optional: Build test executables
option(BUILD_TESTS "Build test executables" ON)

# Power control tool
add_executable(avr-power
    src/tools/avr-power.cpp
)
target_link_libraries(avr-power ${ALSA_LIBRARIES})
target_compile_options(avr-power PRIVATE -Wall -Wextra -Wpedantic -O2)

install(TARGETS avr-power
    RUNTIME DESTINATION bin
)

# Connection broker daemon: owns the one real backend connection per device and
# serves clients (the ALSA plugins, avr-power, amixer) over a Unix socket.
add_executable(avr-controld
    src/daemon/avr_controld.cpp
    ${BACKEND_SOURCES}
    ${CORE_SOURCES}
)
target_link_libraries(avr-controld ${ALSA_LIBRARIES} Threads::Threads)
target_compile_options(avr-controld PRIVATE -Wall -Wextra -Wpedantic -O2)
install(TARGETS avr-controld
    RUNTIME DESTINATION bin
)
# tmpfiles.d entry so /run/avr-controld exists (0777) for cross-user sockets
install(FILES systemd/tmpfiles.d/avr-controld.conf
    DESTINATION lib/tmpfiles.d
)

if(BUILD_TESTS)
    # Prefer a system GoogleTest (libgtest-dev/libgmock-dev) so CI needn't clone
    # from github; fall back to fetching it if not installed.
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        # For Windows: Prevent overriding the parent project's compiler/linker settings
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    # Enable testing
    enable_testing()

    # Unit tests (using Google Test)
    add_executable(unit_tests
        tests/unit/test_denon_protocol.cpp
        tests/unit/test_volume_scaling.cpp
        src/backends/denon_ip.cpp
        src/config.cpp
    )

    target_link_libraries(unit_tests
        GTest::gtest_main
        GTest::gmock
        ${ALSA_LIBRARIES}
        Threads::Threads
    )

    include(GoogleTest)
    gtest_discover_tests(unit_tests)

    # Integration tests (using Google Test)
    add_executable(integration_tests
        tests/integration/test_denon_connection.cpp
        ${BACKEND_SOURCES}
        ${CORE_SOURCES}
        src/shared_state_stub.cpp
    )

    target_link_libraries(integration_tests
        GTest::gtest_main
        ${ALSA_LIBRARIES}
        Threads::Threads
    )

    gtest_discover_tests(integration_tests)

    # Broker tests: drive the real SocketClientBackend against an auto-spawned
    # avr-controld + the mock Denon. Needs the mock harness/env, so it is NOT
    # gtest_discover'd (run via tests/broker/run_broker_tests.sh instead).
    add_executable(broker_tests
        tests/broker/broker_tests.cpp
        src/backends/socket_client.cpp
    )
    target_link_libraries(broker_tests
        GTest::gtest_main
        ${ALSA_LIBRARIES}
        Threads::Threads
    )
    # broker_tests auto-spawns avr-controld, so make sure the daemon is built too
    add_dependencies(broker_tests avr-controld)
endif()
