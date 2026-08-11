# Engine/client/server layering enforcement (#559 / #713).
#
# The monorepo is kept *split-ready* (2026-06-28 decision record): a future
# fl-engine/fl-client/fl-server repo split must be nearly free, so the module boundaries a split
# would cut are asserted here at configure time. The policy is documented in docs/developer/architecture.md
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
#   4. Transport facade (direct): the game binary, `fl-server` and both product libraries must not
#      link `platform-enet` / `platform-gns` directly — they go through the `createNetwork()` facade
#      (`platform-net`), which reaches the backends transitively by design (so this rule is
#      direct-links-only). Locks in the #507 HAL-leak fix. The load tools are exempt (the rule names
#      only the binaries and the two product libs): net_check links platform-enet directly (it IS the
#      enet6 regression instrument), and bot_swarm links the platform-net facade so it can select
#      enet6 (its default) or GNS via --transport for the #649 GNS scale gate.
#   5. Product separation (transitive, #1067 / D23): `game-client` may not reach `fl-server-lib`, and
#      `fl-server-lib` may not reach `game-client`. Code both products need is promoted to `engine-*`;
#      it never lives in one product library and gets reached from the other. Before the product
#      libraries existed there was nothing to police — `game/` and `server/` were bare
#      add_executable() calls, so this guard could not see either binary's code at all.
#   6. Headless server library (transitive): `fl-server-lib` reaches no client backend, for the same
#      reason `fl-server` does not. Stated on the library so it holds in configs that build the
#      library without the executable, and so the failure names the library that introduced the edge.
#   7. Platform floor (transitive): no `platform-*` target may reach an `engine-*` target. platform/
#      is the HAL — it defines interfaces the engine consumes, so an edge back into the engine
#      inverts the layering. This rule was policy in docs and in review only; it is a mechanism now.
#
# fl_assert_layering() is armed from the root CMakeLists.txt via cmake_language(DEFER CALL ...) so
# it runs after every add_subdirectory() — all targets exist, including conditional ones
# (platform-vulkan without an SDK, platform-gns when FL_ENABLE_GNS=OFF). It first runs a self-test
# of the graph walker against synthetic targets, so a refactor that silently breaks the walker
# fails configure instead of passing forever. The self-test also builds a synthetic
# game-client -> fl-server-lib edge and asserts the product-separation rule catches it, so rule 5
# cannot rot into a no-op if the real targets are ever renamed.

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

# Rule 5's body, factored out so the self-test can drive the real code path with synthetic targets:
# neither product library may reach the other. Sets <out_var> to a list of violation strings (empty
# when clean). Absent targets are skipped, so a lean configure that builds only one product is fine.
function(_fl_product_separation_violations a b out_var)
    set(_found "")
    foreach(_pair "${a};${b}" "${b};${a}")
        list(GET _pair 0 _from)
        list(GET _pair 1 _to)
        if(TARGET "${_from}" AND TARGET "${_to}")
            _fl_assert_reaches_none("${_from}" "^${_to}$" 0 _chain)
            if(_chain)
                list(APPEND _found
                    "[product separation] ${_from} must not reach ${_to} — promote the shared code to engine-*: ${_chain}")
            endif()
        endif()
    endforeach()
    set(${out_var} "${_found}" PARENT_SCOPE)
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

    # Rule 5 (#1067): a synthetic client -> server product edge must be reported, and only in that
    # direction. This drives _fl_product_separation_violations itself, so the rule cannot decay into a
    # no-op — the failure mode it guards against is the real target names being renamed out from under
    # a hand-written check that then passes forever.
    add_library(fl-layering-selftest-client INTERFACE)
    add_library(fl-layering-selftest-server INTERFACE)
    target_link_libraries(fl-layering-selftest-client INTERFACE fl-layering-selftest-server)
    _fl_product_separation_violations(
        fl-layering-selftest-client fl-layering-selftest-server _product_violations)
    list(LENGTH _product_violations _n)
    if(NOT _n EQUAL 1)
        message(FATAL_ERROR
            "layering guard self-test: product-separation rule reported ${_n} violation(s) for one "
            "synthetic client -> server edge, expected exactly 1 (got '${_product_violations}').")
    endif()
    if(NOT _product_violations MATCHES "fl-layering-selftest-client must not reach fl-layering-selftest-server")
        message(FATAL_ERROR
            "layering guard self-test: product-separation rule reported the wrong direction "
            "(got '${_product_violations}').")
    endif()
endfunction()

# Deferred entry point: enforce every rule, collect all violations, fail configure with the
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

    # Rules 3 + 6 — headless server: neither fl-server nor its product library reaches a client
    # backend. Naming the library too means the failure points at the target that introduced the edge,
    # and the rule still holds in a configure that builds fl-server-lib without the executable.
    set(_client_deny
        "^(platform-sdl3|platform-vulkan|platform-gui|platform-openal)$|^SDL3::|^Vulkan::|^OpenAL::|^imgui$")
    foreach(_srv fl-server fl-server-lib)
        if(TARGET "${_srv}")
            _fl_assert_reaches_none("${_srv}" "${_client_deny}" 0 _chain)
            if(_chain)
                list(APPEND _violations
                    "[headless server] ${_srv} must not reach a client backend: ${_chain}")
            endif()
        endif()
    endforeach()

    # Rule 4 — transport facade: the binaries and the two product libraries select transports at
    # runtime via createNetwork() (platform-net); none may link a concrete transport backend directly.
    # The product libraries are named explicitly because they are where the sources now live: a
    # depth-1 check on the executables alone would stop seeing the client/server code entirely.
    foreach(_bin fighters-legacy fl-server game-client fl-server-lib)
        if(TARGET "${_bin}")
            _fl_assert_reaches_none("${_bin}" "^(platform-enet|platform-gns)$" 1 _chain)
            if(_chain)
                list(APPEND _violations
                    "[transport facade] ${_bin} must link platform-net, not a concrete transport backend: ${_chain}")
            endif()
        endif()
    endforeach()

    # Rule 5 — product separation (D23): neither product library reaches the other. Shared code is
    # promoted to engine-*, which is the ruling this rule enforces rather than restates.
    _fl_product_separation_violations(game-client fl-server-lib _product_violations)
    if(_product_violations)
        list(APPEND _violations ${_product_violations})
    endif()

    # Rule 7 — platform floor: no platform-* target reaches an engine-* target. platform/ defines the
    # interfaces the engine consumes; an edge back into engine/ inverts the layering.
    foreach(_t IN LISTS _all)
        if(_t MATCHES "^platform-")
            _fl_assert_reaches_none("${_t}" "^engine-" 0 _chain)
            if(_chain)
                list(APPEND _violations
                    "[platform floor] platform targets must not reach engine targets: ${_chain}")
            endif()
        endif()
    endforeach()

    if(_violations)
        set(_msg "Module-boundary layering violation(s) detected:\n")
        foreach(_v IN LISTS _violations)
            string(APPEND _msg "  ${_v}\n")
        endforeach()
        string(APPEND _msg
            "See 'Module Boundary Policy' in docs/developer/architecture.md (enforced by cmake/layering.cmake).")
        message(FATAL_ERROR "${_msg}")
    endif()
endfunction()
