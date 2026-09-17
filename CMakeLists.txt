# libnetmidi2 — Network MIDI 2.0 (UDP) core.
#
# Header-only for now (Protocol.h / Platform.h), so this is an INTERFACE target.
# When Session.cpp and the adapters land, switch to a STATIC library:
#   add_library(netmidi2 STATIC src/Session.cpp)
#   target_include_directories(netmidi2 PUBLIC include)
#
# The core is intentionally freestanding-friendly (no exceptions/RTTI/heap/STL
# containers) so the same code compiles on the Teensy/Zephyr side.

cmake_minimum_required(VERSION 3.15)
project(netmidi2 LANGUAGES CXX)

add_library(netmidi2 INTERFACE)
target_include_directories(netmidi2 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(netmidi2 INTERFACE cxx_std_17)

# Consumers: target_link_libraries(<their_target> PRIVATE netmidi2)

if(NOT NETMIDI2_SKIP_TESTS)
    enable_testing()

    # Conformance vectors — byte-for-byte against M2-124-UM Appendix A.1. Pure unit
    # test over Protocol.h: no sockets, no clock, so it builds anywhere.
    add_executable(nm2_conformance_vectors tests/conformance_vectors.cpp)
    target_link_libraries(nm2_conformance_vectors PRIVATE netmidi2)
    add_test(NAME nm2_conformance_vectors COMMAND nm2_conformance_vectors)

    # Guard tests — the reject paths for malformed/oversized input. Also unit-only.
    add_executable(nm2_protocol_guards tests/protocol_guards.cpp)
    target_link_libraries(nm2_protocol_guards PRIVATE netmidi2)
    add_test(NAME nm2_protocol_guards COMMAND nm2_protocol_guards)

    # Loopback test — runs a Host + Client Session over real localhost UDP (POSIX only).
    if(UNIX)
        add_executable(nm2_session_loopback tests/session_loopback.cpp)
        target_link_libraries(nm2_session_loopback PRIVATE netmidi2)
        add_test(NAME nm2_session_loopback COMMAND nm2_session_loopback)
    endif()
endif()
