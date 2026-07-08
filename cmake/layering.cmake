# Engine/client/server layering assertions (issue #559).
#
# Seed helper for the layering-boundary work. The full layer-DAG enforcement (which target may link
# which) is built out in #559 sub-task 3; for now this provides the single guarantee #712 needs.

# fl_assert_zero_dep(<target>): fail configuration if <target> links anything but the C++ stdlib.
# Threads::Threads is the sole permitted dependency (the stdlib's threading facility). Used to lock
# down `engine-protocol` as a zero-dependency wire-protocol seam that clients and tools can consume
# without pulling WorldBroadcaster / engine-entity.
function(fl_assert_zero_dep target)
    get_target_property(_libs ${target} LINK_LIBRARIES)
    if(_libs)
        foreach(_lib IN LISTS _libs)
            if(NOT _lib MATCHES "^Threads::Threads$")
                message(FATAL_ERROR
                    "Layering violation: ${target} must reach only the C++ stdlib/Threads, "
                    "but links '${_lib}'.")
            endif()
        endforeach()
    endif()
endfunction()
