#
# TaskManagerPlus plugin: QuickActions
#

set(HEADERS
    "resource.h"
    "quickactions.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "QuickActions.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "fuzzy.c"
    "palette.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(QuickActions ${ALL_FILES})

target_link_libraries(QuickActions PRIVATE
    user32
)
