/**
 * @file test_main.c
 * @brief Entry point du firmware de test Unity (Lot E.1, mai 2026).
 *
 * Au boot, affiche le menu interactif Unity sur la console serie :
 *   - taper le numero d'un test pour l'executer
 *   - taper `*` pour executer tous les tests
 *   - taper `[<tag>]` (ex: `[crypto]`) pour filtrer par tag
 *
 * Les tests sont declares dans chaque composant via le pattern :
 *
 *   #include "unity.h"
 *   TEST_CASE("nom du test", "[tag]") { ... TEST_ASSERT_*(...); }
 *
 * et leur fichier source est wire au build par le `CMakeLists.txt` du
 * dossier `test/` du composant (registered avec `WHOLE_ARCHIVE`).
 */

#include "unity.h"
#include "unity_test_runner.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "crypto/crypto_init.h"

#include <stdio.h>

void app_main(void)
{
    /* Laisser le temps a la console USB-OTG natif de se stabiliser
     * apres reset avant d'afficher le menu. Sans ce delai, le premier
     * affichage peut etre tronque selon le terminal serie. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Init des sous-systemes globaux que les tests Unity attendent.
     * Reproduit le pre-amble de `app_main` du firmware principal pour
     * les invariants partages :
     *  - crypto_init() : garde C6, sinon ESP_ERR_INVALID_STATE (259)
     *    sur tout appel crypto_generate_keypair / crypto_sign / verify
     */
    esp_err_t crypto_err = crypto_init();
    if (crypto_err != ESP_OK) {
        printf("WARNING: crypto_init() echec : 0x%x — tests crypto KO\n",
               crypto_err);
    }

    printf("\n");
    printf("====================================\n");
    printf("  Mesh Pay — Test Runner (Unity)\n");
    printf("  Tape `*` pour executer tous les tests,\n");
    printf("  ou un numero / un tag entre crochets.\n");
    printf("====================================\n\n");

    unity_run_menu();
}
