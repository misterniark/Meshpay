idf_component_register(
    SRCS "src/dag.c"
         "src/dag_validate.c"
         "src/dag_prune.c"
         "src/dag_merge.c"
    INCLUDE_DIRS "include"
    REQUIRES transaction crypto
)
