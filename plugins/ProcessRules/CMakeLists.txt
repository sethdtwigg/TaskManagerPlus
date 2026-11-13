#
# TaskManagerPlus plugin: ProcessRules
#

set(HEADERS
    "resource.h"
    "processrules.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "ProcessRules.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "engine.c"
    "main.c"
    "options.c"
    "ruledlg.c"
    "rules.c"
    "tab.c"
    "intf.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(ProcessRules ${ALL_FILES})

target_link_libraries(ProcessRules PRIVATE
    user32
)
