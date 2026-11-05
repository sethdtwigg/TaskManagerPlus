#
# TaskManagerPlus plugin: StartupManager
#

set(HEADERS
    "resource.h"
    "startupmanager.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "StartupManager.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "startupenum.c"
    "startuptab.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(StartupManager ${ALL_FILES})

target_link_libraries(StartupManager PRIVATE
    user32
    oleaut32
)
