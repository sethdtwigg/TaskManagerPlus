#
# TaskManagerPlus plugin: NetworkLookup
#

set(HEADERS
    "resource.h"
    "networklookup.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "NetworkLookup.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "ports.c"
    "country.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(NetworkLookup ${ALL_FILES})

target_link_libraries(NetworkLookup PRIVATE
    user32
)
