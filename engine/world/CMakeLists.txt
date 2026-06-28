# World-systems foundation types: faction registry, alert levels, airspace zones.
# Foundation only — AlertSystem (#162) and IWorldAiProvider (#163) build on top of this.
add_library(engine-world STATIC FactionRegistry.cpp)

# Expose engine/ as the public include root so consumers use #include "world/AlertLevel.h".
target_include_directories(engine-world PUBLIC ${CMAKE_SOURCE_DIR}/engine)
# No external deps — pure ISO C++20 stdlib (mirrors engine-spatial).
