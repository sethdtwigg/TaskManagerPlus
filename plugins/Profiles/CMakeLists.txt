#
# TaskManagerPlus plugin: Profiles
#

set(HEADERS
    "resource.h"
    "profiles.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "Profiles.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "store.c"
    "ui.c"
    "profilecore.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(Profiles ${ALL_FILES})

target_link_libraries(Profiles PRIVATE
    user32
)
