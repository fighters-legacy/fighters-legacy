# Engine/client/server layering enforcement (#559 / #713).
#
# The monorepo is kept *split-ready* (2026-06-28 decision record): a future
# fl-engine/fl-client/fl-server repo split must be nearly free, so the module boundaries a split
# would cut are asserted here at configure time. The policy is documented in docs/architecture.md
# ("Module Boundary Policy"); this file is its enforcement. Four rules:
#
#   1. Engine isolation (transitive): no `engine-*` target may reach a client or transport
#      backend (platform-sdl3 / platform-vulkan / platform-openal / platform-enet / platform-gns /
#      platform-net, or the third-party libs behind them). `platform-hal` (interface headers + GLM)
#      and the pure-stdlib platform utility libs are the only allowed platform edges.
#   2. Protocol zero-dep (direct): `engine-protocol` links nothing beyond the C++ stdlib
#      (Threads::Threads is the sole permitted entry — the stdlib's threading facility). This is
#      the seam a repo split would cut; clients and tools consume it without pulling
#      WorldBroadcaster / engine-entity. Direct-only is sufficient: anything it links must itself
#      be on the allowlist, so there is nothing to recurse into.
#   3. Headless server (transitive): `fl-server` may not reach a client backend
#      (platform-sdl3 / platform-vulkan / platform-openal or their third-party libs). Transport
#      backends are legitimate server deps and stay allowed.
#   4. Transport facade (direct): the game binary and `fl-server` must not link `platform-enet` /
#      `platform-gns` directly — they go through the `createNetwork()` facade (`platform-net`),
#      which reaches the backends transitively by design (so this rule is direct-links-only).
#      Locks in the #507 HAL-leak fix. The load tools are exempt (the rule names only the two
#      binaries): net_check links platform-enet directly (it IS the enet6 regression instrument),
#      and bot_swarm links the platform-net facade so it can select enet6 (its default) or GNS
#      via --transport for the #649 GNS scale gate.
#
# fl_assert_layering() is armed from the root CMakeLists.txt via cmake_language(DEFER CALL ...) so
# it runs after every add_subdirectory() — all targets exist, including conditional ones
# (platform-vulkan without an SDK, platform-gns when FL_ENABLE_GNS=OFF). It first runs a self-test
# of the graph walker against synthetic targets, so a refactor that silently breaks the walker
# fails configure instead of passing forever.

# Collect every non-UTILITY buildsystem target under <dir>, recursively.
function(_fl_all_targets out_var dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    set(_result "")
    foreach(_t IN LISTS _targets)
        get_target_property(_type "${_t}" TYPE)
        if(NOT _type STREQUAL "UTILITY")
            list(APPEND _result "${_t}")
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _fl_all_targets(_sub_targets "${_sub}")
        list(APPEND _result ${_sub_targets})
    endforeach()
    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

# BFS the link graph from <root> (LINK_LIBRARIES + INTERFACE_LINK_LIBRARIES of every reached
# target, $<LINK_ONLY:...> wrappers stripped, aliases resolved). If any reached entry matches
# <deny_regex>, set <out_var> to the "root -> ... -> entry" chain; otherwise "". <max_depth> 0 =
# unlimited; 1 = direct links only. Entries are regex-matched raw (so a denied name buried in a
# generator expression still matches) but only existing targets are recursed into — linker flags,
# bare system libs (ws2_32, dl, ...), and unresolvable genex fragments are leaves.
function(_fl_assert_reaches_none root deny_regex max_depth out_var)
    set(${out_var} "" PARENT_SCOPE)
    set(_frontier "${root}")
    set(_visited "${root}")
    set(_depth 0)
    while(_frontier)
        math(EXPR _depth "${_depth} + 1")
        if(max_depth GREATER 0 AND _depth GREATER max_depth)
            return()
        endif()
        set(_next "")
        foreach(_node IN LISTS _frontier)
            set(_real "${_node}")
            get_target_property(_aliased "${_node}" ALIASED_TARGET)
            if(_aliased)
                set(_real "${_aliased}")
            endif()
            set(_entries "")
            get_target_property(_direct "${_real}" LINK_LIBRARIES)
            if(_direct)
                list(APPEND _entries ${_direct})
            endif()
            get_target_property(_iface "${_real}" INTERFACE_LINK_LIBRARIES)
            if(_iface)
                list(APPEND _entries ${_iface})
            endif()
            foreach(_raw IN LISTS _entries)
                string(REGEX REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1" _name "${_raw}")
                if(_name MATCHES "${deny_regex}")
                    # Reconstruct the chain from the offending node back to the root.
                    set(_chain "${_name}")
                    set(_cur "${_node}")
                    while(1)
                        set(_chain "${_cur} -> ${_chain}")
                        if(_cur STREQUAL "${root}")
                            break()
                        endif()
                        string(MAKE_C_IDENTIFIER "${_cur}" _cid)
                        set(_cur "${_parent_${_cid}}")
                    endwhile()
                    set(${out_var} "${_chain}" PARENT_SCOPE)
                    return()
                endif()
                if(TARGET "${_name}" AND NOT _name IN_LIST _visited)
                    list(APPEND _visited "${_name}")
                    string(MAKE_C_IDENTIFIER "${_name}" _cid)
                    set(_parent_${_cid} "${_node}")
                    list(APPEND _next "${_name}")
                endif()
            endforeach()
        endforeach()
        set(_frontier "${_next}")
    endwhile()
endfunction()

# Assert every direct link entry of <target> matches <allow_regex>. On the first entry that does
# not, set <out_var> to "target -> entry"; otherwise "".
function(_fl_assert_links_only target allow_regex out_var)
    set(${out_var} "" PARENT_SCOPE)
    set(_real "${target}")
    get_target_property(_aliased "${target}" ALIASED_TARGET)
    if(_aliased)
        set(_real "${_aliased}")
    endif()
    set(_entries "")
    get_target_property(_direct "${_real}" LINK_LIBRARIES)
    if(_direct)
        list(APPEND _entries ${_direct})
    endif()
    get_target_property(_iface "${_real}" INTERFACE_LINK_LIBRARIES)
    if(_iface)
        list(APPEND _entries ${_iface})
    endif()
    foreach(_raw IN LISTS _entries)
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1" _name "${_raw}")
        if(NOT _name MATCHES "${allow_regex}")
            set(${out_var} "${target} -> ${_name}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Self-test of the walker mechanics against synthetic INTERFACE targets (a -> b -> c). Runs on
# every configure: the guard is pure configure-time logic, so without this a refactor that broke
# the walk would silently disable enforcement. Synthetic names avoid colliding with real backends
# in lean configs (no Vulkan SDK, FL_ENABLE_GNS=OFF).
function(_fl_layering_selftest)
    add_library(fl-layering-selftest-c INTERFACE)
    add_library(fl-layering-selftest-b INTERFACE)
    add_library(fl-layering-selftest-a INTERFACE)
    target_link_libraries(fl-layering-selftest-b INTERFACE fl-layering-selftest-c)
    target_link_libraries(fl-layering-selftest-a INTERFACE fl-layering-selftest-b)

    _fl_assert_reaches_none(fl-layering-selftest-a "^fl-layering-selftest-c$" 0 _chain)
    if(NOT _chain STREQUAL "fl-layering-selftest-a -> fl-layering-selftest-b -> fl-layering-selftest-c")
        message(FATAL_ERROR
            "layering guard self-test: transitive walk broken (got '${_chain}').")
    endif()
    _fl_assert_reaches_none(fl-layering-selftest-a "^fl-layering-selftest-c$" 1 _chain)
    if(_chain)
        message(FATAL_ERROR
            "layering guard self-test: depth limit broken (got '${_chain}').")
    endif()
    _fl_assert_links_only(fl-layering-selftest-a "^fl-layering-selftest-b$" _chain)
    if(_chain)
        message(FATAL_ERROR
            "layering guard self-test: allowlist false positive (got '${_chain}').")
    endif()
    _fl_assert_links_only(fl-layering-selftest-a "^fl-layering-selftest-nomatch$" _chain)
    if(NOT _chain STREQUAL "fl-layering-selftest-a -> fl-layering-selftest-b")
        message(FATAL_ERROR
            "layering guard self-test: allowlist check broken (got '${_chain}').")
    endif()
endfunction()

# Deferred entry point: enforce all four rules, collect every violation, fail configure with the
# full list of offending link chains.
function(fl_assert_layering)
    _fl_layering_selftest()

    set(_violations "")

    # Rule 1 — engine isolation: no engine-* target reaches a client/transport backend.
    # httplib::httplib joins the list with #233: the embedded HTTP SERVER lives in server/fl-server/
    # for the same reason the outbound HTTP client does — an engine target that could serve HTTP
    # would make the engine a network service, and the point of the seam is that it is not one.
    set(_client_transport_deny
        "^(platform-sdl3|platform-vulkan|platform-gui|platform-openal|platform-enet|platform-gns|platform-net|platform-http)$|^SDL3::|^Vulkan::|^OpenAL::|^enet6$|^GameNetworkingSockets::|^CURL::|^httplib::|^imgui$")
    _fl_all_targets(_all "${CMAKE_SOURCE_DIR}")
    list(REMOVE_DUPLICATES _all)
    list(SORT _all)
    foreach(_t IN LISTS _all)
        if(_t MATCHES "^engine-")
            _fl_assert_reaches_none("${_t}" "${_client_transport_deny}" 0 _chain)
            if(_chain)
                list(APPEND _violations
                    "[engine isolation] engine targets must not reach client/transport backends: ${_chain}")
            endif()
        endif()
    endforeach()

    # Rule 2 — protocol zero-dep: engine-protocol reaches only the C++ stdlib / Threads::Threads.
    if(TARGET engine-protocol)
        _fl_assert_links_only(engine-protocol "^Threads::Threads$" _chain)
        if(_chain)
            list(APPEND _violations
                "[protocol zero-dep] engine-protocol must reach only the C++ stdlib/Threads: ${_chain}")
        endif()
    endif()

    # Rule 3 — headless server: fl-server reaches no client backend.
    set(_client_deny
        "^(platform-sdl3|platform-vulkan|platform-gui|platform-openal)$|^SDL3::|^Vulkan::|^OpenAL::|^imgui$")
    if(TARGET fl-server)
        _fl_assert_reaches_none(fl-server "${_client_deny}" 0 _chain)
        if(_chain)
            list(APPEND _violations
                "[headless server] fl-server must not reach a client backend: ${_chain}")
        endif()
    endif()

    # Rule 4 — transport facade: the game binary and fl-server select transports at runtime via
    # createNetwork() (platform-net); neither may link a concrete transport backend directly.
    foreach(_bin fighters-legacy fl-server)
        if(TARGET "${_bin}")
            _fl_assert_reaches_none("${_bin}" "^(platform-enet|platform-gns)$" 1 _chain)
            if(_chain)
                list(APPEND _violations
                    "[transport facade] ${_bin} must link platform-net, not a concrete transport backend: ${_chain}")
            endif()
        endif()
    endforeach()

    if(_violations)
        set(_msg "Module-boundary layering violation(s) detected:\n")
        foreach(_v IN LISTS _violations)
            string(APPEND _msg "  ${_v}\n")
        endforeach()
        string(APPEND _msg
            "See 'Module Boundary Policy' in docs/architecture.md (enforced by cmake/layering.cmake).")
        message(FATAL_ERROR "${_msg}")
    endif()
endfunction()
