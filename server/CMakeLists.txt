# persistence before fl-server: fl-server-lib links fl-server-persistence, so the target has to
# exist by the time that link line is written.
add_subdirectory(persistence)
add_subdirectory(fl-server)
