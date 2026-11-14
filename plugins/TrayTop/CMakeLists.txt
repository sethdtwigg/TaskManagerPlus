#
# TaskManagerPlus plugin: TrayTop
#

set(HEADERS
    "resource.h"
    "traytop.h"
    "topn.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "TrayTop.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "menu.c"
    "options.c"
    "topn.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(TrayTop ${ALL_FILES})

target_link_libraries(TrayTop PRIVATE
    user32
)
