# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build-system helpers shared across the tree (#1265).

# ---------------------------------------------------------------------------
# fl_add_test(<name> [LIBS <lib>...] [INCLUDES <dir>...] [SERIAL])
# ---------------------------------------------------------------------------
# One Catch2 test executable: <name> built from <name>.cpp, linked against
# Catch2::Catch2WithMain plus LIBS, and registered with catch_discover_tests.
#
# tests/CMakeLists.txt held 260 hand-written copies of that four-line shape, and the copy-paste had
# already drifted in ways that are invisible until someone reads carefully: two stanzas had their
# catch_discover_tests displaced below the NEXT test's whole block, and Catch2WithMain appeared
# sometimes first and sometimes last in the link line. Neither breaks anything today; both are the
# kind of thing that eventually registers a test under the wrong target or silently stops
# registering one at all.
#
# The repo already accepts this idiom -- fuzz/CMakeLists.txt's fl_add_fuzzer collapses exactly the
# same shape for the libFuzzer harnesses.
#
# SERIAL sets RUN_SERIAL on the discovered tests, for a test that measures WALL-CLOCK time and would
# be made flaky by a loaded machine (test_loop sleeps and asserts a rate).
#
# Genuinely special targets -- the mission harnesses' add_test blocks, the Windows DLL copy for
# test_audio, anything taking FL_SERVER_BIN -- stay written out longhand. This helper is for the
# shape that is the same 230 times, not a frame to force the rest into.
function(fl_add_test name)
    cmake_parse_arguments(FL_TEST "SERIAL" "" "LIBS;INCLUDES" ${ARGN})
    if(FL_TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fl_add_test(${name}): unexpected arguments: ${FL_TEST_UNPARSED_ARGUMENTS}")
    endif()

    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain ${FL_TEST_LIBS})
    if(FL_TEST_INCLUDES)
        target_include_directories(${name} PRIVATE ${FL_TEST_INCLUDES})
    endif()
    if(FL_TEST_SERIAL)
        catch_discover_tests(${name} PROPERTIES RUN_SERIAL TRUE)
    else()
        catch_discover_tests(${name})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# fl_quiet_target(<target>...)
# ---------------------------------------------------------------------------
# Exempt third-party targets from our warning discipline: COMPILE_WARNING_AS_ERROR OFF plus the
# compiler-appropriate blanket suppression (-w for GCC/Clang, /W0 for MSVC).
#
# Both halves are needed and they do different things. COMPILE_WARNING_AS_ERROR OFF stops our
# -Werror reaching the target; -w //W0 stops the warnings appearing in the log at all, so a real
# warning in OUR code is not buried under a vendored library's. Fifteen sites set the property and
# seven of those also spelled out the genex, in slightly different forms — one listed
# GNU,Clang,AppleClang, another used $<IF:...MSVC...>.
#
# This is for a target WE do not own. Never point it at an fl- target: the warnings are the point
# there.
#
# ⚠ Targets only. Some vendored code arrives as a single TU compiled into one of our own targets
# (engine's TerrainChunkIO.cpp, campaign/FrontlinePng.cpp), and those must stay
# set_source_files_properties — quieting the whole target would silence our code alongside the
# vendored header.
function(fl_quiet_target)
    foreach(target IN LISTS ARGV)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "fl_quiet_target: no such target '${target}'")
        endif()
        set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
        get_target_property(_fl_type ${target} TYPE)
        if(NOT _fl_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${target} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
                $<$<CXX_COMPILER_ID:MSVC>:/W0>
            )
        endif()
    endforeach()
endfunction()
