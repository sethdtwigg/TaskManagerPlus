#
# TaskManagerPlus plugin: ResourceTrends
#

set(HEADERS
    "resource.h"
    "resourcetrends.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "ResourceTrends.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "trendfmt.c"
    "leak.c"
    "store.c"
    "tracker.c"
    "tab.c"
    "intf.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(ResourceTrends ${ALL_FILES})

target_link_libraries(ResourceTrends PRIVATE
    user32
)
