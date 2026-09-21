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

# Interop bench harness. Not a test -- it needs a real peer on the network -- but
# the thing that found the defects the tests could not, so it ships with the library.
option(NETMIDI2_BUILD_TOOLS "Build the nm2_bench / nm2_cli tools" OFF)
if(NETMIDI2_BUILD_TOOLS AND UNIX)
    add_executable(nm2_bench tools/nm2_bench.cpp)
    target_link_libraries(nm2_bench PRIVATE netmidi2)

    # nm2_cli additionally carries a real mDNS adapter, over Bonjour. macOS has it
    # in libSystem; elsewhere it needs Avahi's Bonjour compatibility layer. Without
    # it the tool still builds and works, just with explicit host:port only.
    add_executable(nm2_cli tools/nm2_cli.cpp)
    target_link_libraries(nm2_cli PRIVATE netmidi2)
    if(NOT APPLE)
        # Avahi's compat layer installs the header in its own subdirectory, not
        # straight into /usr/include, so it needs the suffix hint to be found.
        find_path(DNSSD_INCLUDE_DIR dns_sd.h PATH_SUFFIXES avahi-compat-libdns_sd)
        find_library(DNSSD_LIBRARY NAMES dns_sd)
        if(DNSSD_INCLUDE_DIR AND DNSSD_LIBRARY)
            target_include_directories(nm2_cli PRIVATE ${DNSSD_INCLUDE_DIR})
            target_link_libraries(nm2_cli PRIVATE ${DNSSD_LIBRARY})
            target_compile_definitions(nm2_cli PRIVATE HAVE_DNS_SD=1)
        else()
            message(STATUS "nm2_cli: no dns_sd found -- building without mDNS")
        endif()
    endif()
endif()

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

    # Socket-backed tests — real localhost UDP (POSIX only).
    if(UNIX)
        add_executable(nm2_session_loopback tests/session_loopback.cpp)
        target_link_libraries(nm2_session_loopback PRIVATE netmidi2)
        add_test(NAME nm2_session_loopback COMMAND nm2_session_loopback)

        # One Host port serving several Clients (spec 3.2).
        add_executable(nm2_host_multiclient tests/host_multiclient.cpp)
        target_link_libraries(nm2_host_multiclient PRIVATE netmidi2)
        add_test(NAME nm2_host_multiclient COMMAND nm2_host_multiclient)

        # mDNS / DNS-SD discovery (spec 4), driven through a fake adapter.
        add_executable(nm2_discovery tests/discovery.cpp)
        target_link_libraries(nm2_discovery PRIVATE netmidi2)
        add_test(NAME nm2_discovery COMMAND nm2_discovery)

        # FEC sending (spec 7.2.2) — repeat ordering, size limit, idle interaction.
        add_executable(nm2_fec_sending tests/fec_sending.cpp)
        target_link_libraries(nm2_fec_sending PRIVATE netmidi2)
        add_test(NAME nm2_fec_sending COMMAND nm2_fec_sending)

        # Retransmit (spec 7.2.3 / 7.2.4) — serving requests, asking, NAK handling.
        add_executable(nm2_retransmit tests/retransmit.cpp)
        target_link_libraries(nm2_retransmit PRIVATE netmidi2)
        add_test(NAME nm2_retransmit COMMAND nm2_retransmit)

        # Authentication (spec 6.7-6.10) — digests vs the spec's worked examples.
        add_executable(nm2_auth tests/auth.cpp)
        target_link_libraries(nm2_auth PRIVATE netmidi2)
        add_test(NAME nm2_auth COMMAND nm2_auth)

        # Session Reset (spec 6.11 / 6.12) — the exchange, and clearing BOTH counters.
        add_executable(nm2_session_reset tests/session_reset.cpp)
        target_link_libraries(nm2_session_reset PRIVATE netmidi2)
        add_test(NAME nm2_session_reset COMMAND nm2_session_reset)
    endif()
endif()
