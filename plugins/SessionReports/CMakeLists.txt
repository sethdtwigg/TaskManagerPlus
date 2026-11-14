#
# TaskManagerPlus plugin: SessionReports
#

set(HEADERS
    "resource.h"
    "sessionreports.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "SessionReports.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "collect.c"
    "export.c"
    "cli.c"
    "cliparse.c"
    "reportfmt.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(SessionReports ${ALL_FILES})

target_link_libraries(SessionReports PRIVATE
    user32
)
