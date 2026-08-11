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
# Callers add target_link_libraries / target_include_directories AFTER the call
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

# Lobby server-list JSON scanner: fl::parseLobbyServerList (#143).
fl_add_fuzzer(fuzz_lobby_list)
target_link_libraries(fuzz_lobby_list PRIVATE engine-net)

# REST admin API request-body scanners (#233): the bytes a POST /kick or /ban carries. The pure
# helpers moved into their own TU with #601, so this harness no longer drags in an HTTP server to
# fuzz a field scanner.
fl_add_fuzzer(fuzz_http_admin_body)
# fl-server-lib carries HttpAdminAuth plus McpProtocol (parseAutonomy, which buildTokenTable resolves
# per token row). Linking the archive pulls only the objects the harness actually reaches (#1067).
target_link_libraries(fuzz_http_admin_body PRIVATE fl-server-lib)

# MCP JSON-RPC envelope + depth-aware member scanner (#601): the first code to touch an MCP request
# body. Pure logic, no sockets — McpProtocol.cpp links nothing but engine-net's jsonEscape.
fl_add_fuzzer(fuzz_mcp)
target_link_libraries(fuzz_mcp PRIVATE fl-server-lib)

# Source-Engine RCON packet decoder: fl::rcon::decodePacket, from fl-server-lib (#1067 — the library
# carries the Winsock link edge too, so the WIN32 branch this harness needed is gone).
fl_add_fuzzer(fuzz_rcon_packet)
target_link_libraries(fuzz_rcon_packet PRIVATE fl-server-lib)

# --- v2 message-handler harnesses (#709; stateful, over the tests/ mocks — no real SDL/ENet) ---

# WorldBroadcaster::onReceive — the server's untrusted client-packet path (ClientInput/AdminCommand/
# Heartbeat). Uses TrackingNetwork + a real EntityManager; the tests/ dir is on the include path for
# mock_network.h. FuzzFrames.h splits the input into ordered onReceive() packets.
fl_add_fuzzer(fuzz_server_msg)
target_link_libraries(fuzz_server_msg PRIVATE engine-net)
target_include_directories(fuzz_server_msg PRIVATE "${CMAKE_SOURCE_DIR}/tests")

# The connect path + everything behind the handshake gate (#1073). fuzz_server_msg never completes a
# handshake, so handleConnectRequest -- the largest untrusted parser in the server -- and every
# handler gated on handshakeComplete were unreachable from it.
fl_add_fuzzer(fuzz_connect_path)
target_link_libraries(fuzz_connect_path PRIVATE engine-net)
target_include_directories(fuzz_connect_path PRIVATE "${CMAKE_SOURCE_DIR}/tests")

# ClientNetEventHandler::onReceive — the widest untrusted-input surface on the client. It dispatches
# MsgWingmanAck into the radio menu (#610), routes the effects TLV through ClientEffectRouter (#625),
# and the router drives own-ship haptics (#631); game-client carries every one of them.
fl_add_fuzzer(fuzz_client_msg)
target_link_libraries(fuzz_client_msg PRIVATE game-client)
target_include_directories(fuzz_client_msg PRIVATE "${CMAKE_SOURCE_DIR}/tests")

# --- v2 content / asset parser harnesses (#709; mostly pure) ---

# AssetValidator::validate — magic-byte + size gate (first fuzz byte selects the AssetType).
fl_add_fuzzer(fuzz_asset_validator)
target_link_libraries(fuzz_asset_validator PRIVATE engine-content)

# .flrep replay reader (#643): a file downloaded from a stranger, per docs/developer/replay-format.md §5.
fl_add_fuzzer(fuzz_flrep)
target_link_libraries(fuzz_flrep PRIVATE engine-replay)

# decodeTerrainChunkPng — 16-bit grayscale PNG decoder (stb_image).
fl_add_fuzzer(fuzz_terrain_png)
target_link_libraries(fuzz_terrain_png PRIVATE engine-render)

# decodeOgg + the OGG streaming API — the untrusted content-pack audio path, backed by libvorbis
# (vorbisfile) since #723 (the prior stb_vorbis backend crashed on malformed input and had this
# harness parked in fuzz/disabled/).
fl_add_fuzzer(fuzz_ogg)
target_link_libraries(fuzz_ogg PRIVATE engine-audio)

# parseFlightModel — flight-model TOML parser (throws; harness catches).
fl_add_fuzzer(fuzz_flight_model_toml)
target_link_libraries(fuzz_flight_model_toml PRIVATE engine-flight)

# parseEntityDef — entity-definition TOML parser (throws; harness catches).
fl_add_fuzzer(fuzz_entity_def_toml)
target_link_libraries(fuzz_entity_def_toml PRIVATE engine-entity)

# parseWeaponDef — weapon TOML parser (throws; harness catches).
fl_add_fuzzer(fuzz_weapon_def_toml)
target_link_libraries(fuzz_weapon_def_toml PRIVATE engine-weapon)

# parseSensorDef — sensor TOML parser (throws; harness catches).
fl_add_fuzzer(fuzz_sensor_def_toml)
target_link_libraries(fuzz_sensor_def_toml PRIVATE engine-sensor)

# parsePlaylist — music-playlist TOML parser (needs an ILogger&).
fl_add_fuzzer(fuzz_playlist_toml)
target_link_libraries(fuzz_playlist_toml PRIVATE engine-audio)

# parseServerConfig — fl-server config TOML parser (nullable logger; already proven fuzz-linkable).
fl_add_fuzzer(fuzz_server_config_toml)
target_link_libraries(fuzz_server_config_toml PRIVATE fl-server-config)
target_include_directories(fuzz_server_config_toml PRIVATE "${CMAKE_SOURCE_DIR}/server/fl-server")

# ModLoader::load — content-pack discovery + manifest.toml parse + name sanitizers, over MockFilesystem.
fl_add_fuzzer(fuzz_mod_manifest)
target_link_libraries(fuzz_mod_manifest PRIVATE engine-content)
target_include_directories(fuzz_mod_manifest PRIVATE "${CMAKE_SOURCE_DIR}/tests")

# validateMeshFromJson — in-memory glTF 2.0 validator (tinygltf; graphics-free lib).
fl_add_fuzzer(fuzz_mesh_json)
target_link_libraries(fuzz_mesh_json PRIVATE validate-mesh-lib)

# --- Seed-corpus mint tool (NOT a fuzzer: own main, no -fsanitize=fuzzer, no ctest) ---
# Regenerates the committed corpus/<name>/seed-*.bin from the same builders the unit tests use.
# Run once after building: ./build/fuzz/fuzz/fuzz-mint-seeds
add_executable(fuzz-mint-seeds mint_seeds.cpp)
target_link_libraries(fuzz-mint-seeds PRIVATE engine-protocol engine-replay fl-server-lib)
target_include_directories(fuzz-mint-seeds PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(fuzz-mint-seeds PRIVATE FL_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
