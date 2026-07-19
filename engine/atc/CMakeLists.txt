# Air-traffic-control service core (#702, epic #673): facilities, runway sequencing, clearances.
# The behavior layer sits above entity + ai (like engine-script); engine-world stays a pure-stdlib
# foundation below it. A pure deterministic FSM — no model involvement anywhere.
add_library(engine-atc STATIC
    AtcTypes.cpp
    AtcFacility.cpp
    AtcService.cpp
)

# Expose engine/ as the public include root so consumers use #include "atc/AtcService.h" etc.
target_include_directories(engine-atc PUBLIC ${CMAKE_SOURCE_DIR}/engine)

target_link_libraries(engine-atc
    PUBLIC engine-world   # AirportRegistry / ResolvedAirport
           engine-entity  # EntityManager / EntityState / EntityId
    PRIVATE engine-ai     # AtcBehaviors compositions + LocalFrame guidance math
)
