# Server-side AI autopilot controllers.
add_library(engine-ai STATIC
    LoiterController.cpp
    DynamicLoiterController.cpp
    WaypointController.cpp
    PursuitController.cpp
    EvadeController.cpp
    BreakTurnController.cpp
    StateMachineController.cpp
    LeadPursuitController.cpp
    LagPursuitController.cpp
    ImmelmannController.cpp
    SplitSController.cpp
    HighYoYoController.cpp
    BallisticGuidanceController.cpp
    GunsEmploymentController.cpp
    LowYoYoController.cpp
    TakeoffController.cpp
    LandingController.cpp
    FormationController.cpp
    SwarmController.cpp
    SurfaceThreatControllers.cpp
    TurretGunnerController.cpp
    Threat.cpp
    WingmanBehavior.cpp
)

# Expose engine/ as the public include root so consumers can use
# #include "ai/Guidance.h", #include "ai/WaypointController.h", etc.
target_include_directories(engine-ai PUBLIC ${CMAKE_SOURCE_DIR}/engine)

target_link_libraries(engine-ai
    PRIVATE engine-entity
            engine-flight
            engine-world
            engine-sensor   # sensing-gated conditions + TargetView read ctx.contacts (#690)
)
