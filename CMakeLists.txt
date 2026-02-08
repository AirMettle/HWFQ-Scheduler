cmake_minimum_required(VERSION 3.14)
project(hwfq C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(hwfq STATIC
    src/hwfq_config.c
    src/hwfq_memory.c
    src/hwfq_group_scheduler.c
    src/hwfq_group_calendar.c
    src/hwfq_flow_trie.c
    src/hwfq_chunked_entries.c
    src/hwfq_scheduler.c
)

target_include_directories(hwfq
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hwfq PUBLIC pthread m)

install(TARGETS hwfq
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)
