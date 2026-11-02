idf_component_register(SRCS "cvbs_sync.c" "cvbs_line.c"
                       INCLUDE_DIRS "include")
# Hot loops: keep them fast even in debug-optimised builds.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)
