# Speech-to-text HAL backend (#935) — the deterministic voice-command tier.
#
# ALWAYS builds; the whisper.cpp dependency is what FL_ENABLE_WHISPER gates. A lean build therefore
# links this target exactly the same way and gets NullSpeechToText, which is the same shape
# platform-http uses for its libcurl-optional backend: one target, one factory, one degradation path.
add_library(platform-stt STATIC
    WhisperSpeechToText.cpp
)
target_include_directories(platform-stt PUBLIC
    "${CMAKE_SOURCE_DIR}/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(platform-stt PUBLIC platform-hal)

if(FL_ENABLE_WHISPER AND TARGET whisper)
    target_link_libraries(platform-stt PRIVATE whisper)
    target_compile_definitions(platform-stt PRIVATE FL_HAVE_WHISPER)
    message(STATUS "platform-stt: whisper.cpp backend enabled")
else()
    message(STATUS "platform-stt: no STT backend (voice commands fall back to the radio menu)")
endif()
