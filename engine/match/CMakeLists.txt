# Multiplayer match framework (#497): data-driven game modes, team balancing, the match-lifecycle
# state machine, and AI bot-fill policy. Pure logic — no networking, entity, or platform dependency;
# fl-server owns the MatchController and wires it into WorldBroadcaster via std::function/POD seams
# (the engine-mission split precedent), so engine-net never links engine-match.
add_library(engine-match STATIC
    GameModeParser.cpp
    MatchController.cpp
)

# Expose engine/ as the public include root so consumers use #include "match/GameModeDef.h".
target_include_directories(engine-match PUBLIC ${CMAKE_SOURCE_DIR}/engine)

# GameModeDef/TeamBalancer are pure stdlib; the TOML parser needs tomlplusplus PRIVATE. engine-world
# is PUBLIC because later match types reference FactionDef when synthesizing mode teams.
target_link_libraries(engine-match
    PUBLIC  platform-hal
            engine-world
    PRIVATE tomlplusplus::tomlplusplus
)
