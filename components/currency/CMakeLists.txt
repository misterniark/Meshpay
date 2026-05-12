# CMakeLists.txt du composant currency
# Configuration de la monnaie : règles de validation et fonte

idf_component_register(
    SRCS "src/currency_rules.c"
         "src/currency_melt.c"
    INCLUDE_DIRS "include"
    REQUIRES crypto transaction
)
