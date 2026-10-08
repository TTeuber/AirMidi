# The portable core in lib/, built as an ESP-IDF component from the same sources the host tests use.
set(core ${CMAKE_CURRENT_LIST_DIR}/../../../lib)
file(GLOB core_srcs ${core}/src/*.c)
idf_component_register(SRCS ${core_srcs} INCLUDE_DIRS ${core}/include)
