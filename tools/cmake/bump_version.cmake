# =============================================================================
# bump_version.cmake — Generateur du header meshpay_version.h
#
# A chaque appel, ce script :
#   1. lit le fichier `VERSION` (semver gere a la main),
#   2. incremente le compteur `build_number.txt` (local, gitignore),
#   3. recupere le SHA git court de HEAD (+ marqueur "-dirty" si modifs),
#   4. capture la date du build (UTC, format AAAA-MM-JJ),
#   5. genere `meshpay_version.h` avec les macros utilisees par l'UI.
#
# Doit etre appele en mode script :
#     cmake -DMESHPAY_ROOT=<racine> -DOUTPUT_HEADER=<header.h> \
#           -P tools/cmake/bump_version.cmake
#
# Le composant `ui` invoque ce script via un `add_custom_target(... ALL)`
# pour qu'il tourne a chaque build, qu'il y ait ou non du code a recompiler.
# =============================================================================

if(NOT DEFINED MESHPAY_ROOT)
    message(FATAL_ERROR "bump_version.cmake : MESHPAY_ROOT non defini")
endif()
if(NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "bump_version.cmake : OUTPUT_HEADER non defini")
endif()

# --- 1. Lecture du semver ----------------------------------------------------
# Fichier `VERSION` tracke dans git. Format attendu : "MAJEUR.MINEUR.PATCH".
# Bumper a la main quand on change de palier fonctionnel.
set(VERSION_FILE "${MESHPAY_ROOT}/VERSION")
if(EXISTS "${VERSION_FILE}")
    file(READ "${VERSION_FILE}" SEMVER)
    string(STRIP "${SEMVER}" SEMVER)
else()
    set(SEMVER "0.0.0")
    message(WARNING "bump_version.cmake : ${VERSION_FILE} introuvable, semver par defaut 0.0.0")
endif()

# --- 2. Increment du compteur de build ---------------------------------------
# Fichier local gitignore : chaque developpeur a son propre compteur. Il sert
# uniquement a horodater les binaires flashes sur les devices et a tracer les
# tests / regressions ("le bug est apparu apres le build 47").
set(BUILD_COUNTER_FILE "${MESHPAY_ROOT}/build_number.txt")
if(EXISTS "${BUILD_COUNTER_FILE}")
    file(READ "${BUILD_COUNTER_FILE}" BUILD_NUMBER)
    string(STRIP "${BUILD_NUMBER}" BUILD_NUMBER)
else()
    set(BUILD_NUMBER "0")
endif()
# Garde-fou : si le fichier est corrompu (non numerique), on repart de 0
# plutot que de planter le build pour rien.
if(NOT BUILD_NUMBER MATCHES "^[0-9]+$")
    message(WARNING "bump_version.cmake : build_number.txt invalide ('${BUILD_NUMBER}'), reset a 0")
    set(BUILD_NUMBER "0")
endif()
math(EXPR BUILD_NUMBER "${BUILD_NUMBER} + 1")
file(WRITE "${BUILD_COUNTER_FILE}" "${BUILD_NUMBER}\n")

# --- 3. SHA git court + marqueur dirty ---------------------------------------
# Si git est absent ou si le projet n'est pas un repo, on retombe sur "nogit"
# pour ne jamais casser un build qui se ferait hors arbre versionne (zip
# d'archive par exemple).
find_package(Git QUIET)
set(GIT_SHA "nogit")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${MESHPAY_ROOT}" rev-parse --short HEAD
        OUTPUT_VARIABLE GIT_SHA_RAW
        ERROR_QUIET
        RESULT_VARIABLE GIT_RC
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(GIT_RC EQUAL 0 AND GIT_SHA_RAW)
        set(GIT_SHA "${GIT_SHA_RAW}")
        # Detecte un arbre "dirty" (modifs non commitees) — on ignore
        # `build_number.txt` qui est gitignore et donc invisible ici.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${MESHPAY_ROOT}" status --porcelain
            OUTPUT_VARIABLE GIT_STATUS
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(GIT_STATUS)
            set(GIT_SHA "${GIT_SHA}-dirty")
        endif()
    endif()
endif()

# --- 4. Date du build (UTC) --------------------------------------------------
string(TIMESTAMP BUILD_DATE "%Y-%m-%d" UTC)

# --- 5. Chaine human-readable assemblee --------------------------------------
# Format : "v0.1.0 build 47 - 2026-05-15 abc1234"
# Tout ASCII pour eviter les surprises d'encodage dans LVGL.
set(VERSION_STRING "v${SEMVER} build ${BUILD_NUMBER} - ${BUILD_DATE} ${GIT_SHA}")

# --- 6. Generation du header -------------------------------------------------
# On ecrit toujours le fichier (le BUILD_NUMBER change a chaque appel), ce qui
# force la recompilation de ui_screen_admin.c et le re-link de l'image. C'est
# voulu : c'est le seul moyen d'avoir une chaine fraiche dans le binaire.
file(WRITE "${OUTPUT_HEADER}"
"/**
 * @file meshpay_version.h
 * @brief Numero de version et metadata de build — AUTO-GENERE.
 *
 * Ne pas editer a la main : ce fichier est regenere a chaque build
 * par tools/cmake/bump_version.cmake.
 *
 * Le compteur de build est local (gitignore), la version semantique
 * vient du fichier VERSION a la racine du projet.
 */
#pragma once

#define MESHPAY_VERSION_SEMVER  \"${SEMVER}\"
#define MESHPAY_VERSION_BUILD   ${BUILD_NUMBER}
#define MESHPAY_VERSION_DATE    \"${BUILD_DATE}\"
#define MESHPAY_VERSION_GIT     \"${GIT_SHA}\"
#define MESHPAY_VERSION_STRING  \"${VERSION_STRING}\"
")

message(STATUS "MeshPay version : ${VERSION_STRING}")
