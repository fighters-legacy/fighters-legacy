# World systems: faction registry, alert levels, the airspace alert system (#162), the
# formation/command hierarchy (#610), and airport/runway definitions (#699).
# IWorldAiProvider (#163) builds on top of this.
add_library(engine-world STATIC
    FactionRegistry.cpp
    FormationRegistry.cpp
    AlertSystem.cpp
    EscalationPolicyParser.cpp
    AirportRegistry.cpp
    AirportDefParser.cpp
    AirportCsvImport.cpp
    AirportIndexFile.cpp
    AirportBootstrap.cpp
)

# Expose engine/ as the public include root so consumers use #include "world/AlertLevel.h".
target_include_directories(engine-world PUBLIC ${CMAKE_SOURCE_DIR}/engine)

# AirportRegistry's public API uses glm::dvec3 (world positions on the sphere), so platform-hal
# (which carries GLM as an INTERFACE dep) is PUBLIC; the airport TOML parser needs tomlplusplus
# PRIVATE. FactionRegistry/FormationRegistry remain pure stdlib — the deps arrived with #699.
target_link_libraries(engine-world
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)
