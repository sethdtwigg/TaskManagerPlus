#
# TaskManagerPlus plugin: ProcessHistory
#

set(HEADERS
    "resource.h"
    "processhistory.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "ProcessHistory.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "capture.c"
    "store.c"
    "tab.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(ProcessHistory ${ALL_FILES})

target_link_libraries(ProcessHistory PRIVATE
    user32
    tdh
)
