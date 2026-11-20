# SPDX-License-Identifier: GPL-2.0-or-later

# The one switch for the archive of Andarium's Release configuration and the
# device runtimes it embeds: android/lineage-24/metadata/accepted-aot.env says
# whether the accepted AOT artifact may be in them
# (LINEAGE24_ACCEPTED_AOT_ARCHIVES). darwin/cpu/CMakeLists.txt takes the
# accepted artifact into an iOS runtime only when it says "yes"; an artifact a
# build names (UML_AOT_ARTIFACT) is for measuring on a device, and
# scripts/check-embedded-artifacts.py refuses an archive that carries the
# component while it says "no".

# uml_aot_archives_state(<variable> <descriptor>)
#
# Sets <variable> to "yes" or "no" as the descriptor states, or to "none" when
# there is no descriptor (no artifact is accepted, and none may be archived).
function(uml_aot_archives_state variable descriptor)
    if(NOT EXISTS "${descriptor}")
        set(${variable} none PARENT_SCOPE)
        return()
    endif()
    file(STRINGS "${descriptor}" _line
        REGEX "^LINEAGE24_ACCEPTED_AOT_ARCHIVES='.*'$")
    string(REGEX REPLACE "^[A-Z0-9_]+='(.*)'$" "\\1" _state "${_line}")
    if(NOT _state STREQUAL "yes" AND NOT _state STREQUAL "no")
        message(FATAL_ERROR
            "${descriptor} states no LINEAGE24_ACCEPTED_AOT_ARCHIVES of yes "
            "or no")
    endif()
    set(${variable} ${_state} PARENT_SCOPE)
endfunction()
