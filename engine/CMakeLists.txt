# Engine core: content system, asset manager, IContentPack interface
# Sources added here as Workstream A progresses

add_library(engine-content STATIC
    content/FolderContentPack.cpp
    content/ModLoader.cpp
    content/AssetManager.cpp
)

target_include_directories(engine-content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-content
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-input STATIC
    input/InputBindings.cpp
    input/AxisConfig.cpp
)

target_include_directories(engine-input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-input
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-i18n STATIC
    i18n/StringTable.cpp
    i18n/Localization.cpp
)

target_include_directories(engine-i18n PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-i18n
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-config STATIC
    config/UserConfig.cpp
)

target_include_directories(engine-config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-config
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-firstrun STATIC
    firstrun/FirstRun.cpp
)

target_include_directories(engine-firstrun PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-firstrun
    PUBLIC  engine-config
            platform-hal
)

add_library(engine-sandbox STATIC
    sandbox/SandboxInspector.cpp
)

target_include_directories(engine-sandbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-sandbox
    PUBLIC platform-hal
)

add_subdirectory(crash)

add_library(engine-loop STATIC
    loop/GameLoop.cpp
    loop/TimeController.cpp
)
target_include_directories(engine-loop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-loop PUBLIC platform-hal)

add_library(engine-flight STATIC
    flight/Atmosphere.cpp
    flight/FlightModelParser.cpp
    flight/AeroForces.cpp
    flight/FlightIntegrator.cpp
)
target_include_directories(engine-flight PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-flight
    PUBLIC  engine-content
    PRIVATE tomlplusplus::tomlplusplus
)
