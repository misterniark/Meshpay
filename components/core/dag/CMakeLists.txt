set(dag_srcs
    "src/dag.c"
    "src/dag_validate.c"
    "src/dag_prune.c"
    "src/dag_merge.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND dag_srcs "test/test_dag.c")
    idf_component_register(
        SRCS ${dag_srcs}
        INCLUDE_DIRS "include"
        REQUIRES transaction crypto
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${dag_srcs}
        INCLUDE_DIRS "include"
        REQUIRES transaction crypto
    )
endif()
