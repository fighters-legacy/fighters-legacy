# libFuzzer harness scaffold (#708, Sub 1 of #94).
#
# Enabled only via the clang-only `fuzz` preset (FL_BUILD_FUZZERS=ON). The whole tree is compiled
# with `-fsanitize=fuzzer-no-link,address,undefined` (from the preset's CMAKE_CXX_FLAGS), giving
# coverage feedback across every linked library; each harness adds `-fsanitize=fuzzer` at link to
# pull in the libFuzzer main. Adding a harness = one <name>.cpp + one fl_add_fuzzer(<name>) line
# (+ the caller's target_link_libraries), plus tiny synthetic seeds under corpus/<name>/.

# Umbrella target: `cmake --build --preset fuzz --target fuzzers` builds every harness.
add_custom_target(fuzzers)

# fl_add_fuzzer(<name>): build fuzz/<name>.cpp into a libFuzzer executable + a 60 s ctest smoke test
# (label "fuzz") that replays the committed corpus/<name>/ seeds plus a build-tree working corpus.
# Callers add target_link_libraries / target_sources / target_include_directories AFTER the call
# (inline idiom, like tests/CMakeLists.txt) so per-harness link sets differ freely.
function(fl_add_fuzzer name)
    add_executable(${name} ${name}.cpp)
    # Bring in the libFuzzer driver/main (the global compile flags only instrument for coverage).
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    add_dependencies(fuzzers ${name})

    # Per-harness build-tree working corpus (libFuzzer writes newly-discovered inputs here) and a
    # crash-artifact directory. Both must exist before the run.
    set(work_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/${name}")
    set(artifacts "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${name}")
    file(MAKE_DIRECTORY "${work_corpus}")
    file(MAKE_DIRECTORY "${artifacts}")

    set(seed_corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")

    # Optional per-harness dictionary (fuzz/<name>.dict), auto-detected.
    set(dict_arg "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}.dict")
        set(dict_arg "-dict=${CMAKE_CURRENT_SOURCE_DIR}/${name}.dict")
    endif()

    add_test(
        NAME fuzz_smoke_${name}
        COMMAND ${name}
            "${work_corpus}" "${seed_corpus}"
            -max_total_time=60
            -rss_limit_mb=2048
            -malloc_limit_mb=512
            -max_len=1048576
            -print_final_stats=1
            "-artifact_prefix=${artifacts}/"
            ${dict_arg})
    set_tests_properties(fuzz_smoke_${name} PROPERTIES LABELS "fuzz" TIMEOUT 300)
endfunction()

# --- v1 network-codec harnesses (pure, zero-mock) ---

# Snapshot bit-stream decoder: fl::readMsg<MsgWorldSnapshotHeader> + fl::decodeRecord over BitReader.
fl_add_fuzzer(fuzz_snapshot_codec)
target_link_libraries(fuzz_snapshot_codec PRIVATE engine-protocol)

# TLV extension scanner: fl::findExt / fl::readExtValue.
fl_add_fuzzer(fuzz_wire_tlv)
target_link_libraries(fuzz_wire_tlv PRIVATE engine-protocol)

# Source-Engine RCON packet decoder: fl::rcon::decodePacket. RconServer.cpp is not in any library
# (fl-server compiles it directly), so mirror tests/CMakeLists.txt's test_rcon_server recipe.
fl_add_fuzzer(fuzz_rcon_packet)
target_sources(fuzz_rcon_packet PRIVATE "${CMAKE_SOURCE_DIR}/server/fl-server/RconServer.cpp")
target_link_libraries(fuzz_rcon_packet PRIVATE fl-server-config engine-console)
target_include_directories(fuzz_rcon_packet PRIVATE "${CMAKE_SOURCE_DIR}/server/fl-server")
if(WIN32)
    target_link_libraries(fuzz_rcon_packet PRIVATE ws2_32)
endif()

# --- Seed-corpus mint tool (NOT a fuzzer: own main, no -fsanitize=fuzzer, no ctest) ---
# Regenerates the committed corpus/<name>/seed-*.bin from the same builders the unit tests use.
# Run once after building: ./build/fuzz/fuzz/fuzz-mint-seeds
add_executable(fuzz-mint-seeds mint_seeds.cpp "${CMAKE_SOURCE_DIR}/server/fl-server/RconServer.cpp")
target_link_libraries(fuzz-mint-seeds PRIVATE engine-protocol fl-server-config engine-console)
target_include_directories(fuzz-mint-seeds PRIVATE "${CMAKE_SOURCE_DIR}/server/fl-server")
target_compile_definitions(fuzz-mint-seeds PRIVATE FL_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
if(WIN32)
    target_link_libraries(fuzz-mint-seeds PRIVATE ws2_32)
endif()
