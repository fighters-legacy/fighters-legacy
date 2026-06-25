# Server-side AI autopilot controllers.
add_library(engine-ai STATIC
    LoiterController.cpp
    WaypointController.cpp
    PursuitController.cpp
    EvadeController.cpp
    BreakTurnController.cpp
    StateMachineController.cpp
)

# Expose engine/ as the public include root so consumers can use
# #include "ai/Guidance.h", #include "ai/WaypointController.h", etc.
target_include_directories(engine-ai PUBLIC ${CMAKE_SOURCE_DIR}/engine)

target_link_libraries(engine-ai
    PRIVATE engine-entity
            engine-flight
)
