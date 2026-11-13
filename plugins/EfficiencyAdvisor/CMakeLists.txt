#
# TaskManagerPlus plugin: EfficiencyAdvisor
#

set(HEADERS
    "resource.h"
    "effadvisor.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "EfficiencyAdvisor.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "tracker.c"
    "tab.c"
    "advisor.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(EfficiencyAdvisor ${ALL_FILES})

target_link_libraries(EfficiencyAdvisor PRIVATE
    user32
)
