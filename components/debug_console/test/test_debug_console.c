/**
 * @file test_debug_console.c
 * @brief Tests Unity du composant debug_console.
 *
 * Exerce uniquement les helpers purs (sans IO) :
 *   - debug_console_parse() : reconnait les commandes, ignore casse,
 *     accepte CR/LF/espaces, retourne EMPTY pour ligne vide.
 *   - debug_console_hex_encode() : encode binaire en hex lowercase,
 *     refuse les buffers trop petits.
 *
 * La tache UART et le dispatching de callbacks ne sont pas testes
 * ici — ce sont des comportements d'integration, valides au smoke
 * test hardware avec le moniteur Python.
 */

#include "sdkconfig.h"

#if CONFIG_MESHPAY_DEBUG_CONSOLE

#include "unity.h"
#include "debug_console/debug_console.h"

#include <string.h>

/* ================================================================
 * Tests du parseur
 * ================================================================ */

TEST_CASE("debug_console_parse_reconnait_chaque_commande",
          "[debug_console][parser]")
{
    TEST_ASSERT_EQUAL(DEBUG_CMD_HELP,          debug_console_parse("help"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG,      debug_console_parse("dump_dag"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_WALLET,   debug_console_parse("dump_wallet"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_CURRENCY, debug_console_parse("dump_currency"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_TIME,     debug_console_parse("dump_time"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_ALL,      debug_console_parse("dump_all"));
}

TEST_CASE("debug_console_parse_ignore_la_casse",
          "[debug_console][parser]")
{
    /* Un moniteur qui tape DUMP_DAG ou Dump_Dag doit fonctionner. */
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG, debug_console_parse("DUMP_DAG"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG, debug_console_parse("Dump_Dag"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_HELP,     debug_console_parse("HELP"));
}

TEST_CASE("debug_console_parse_trimme_espaces_et_crlf",
          "[debug_console][parser]")
{
    /* fgets() laisse le '\n' final et parfois le CR sur Windows.
     * Le parser doit l'absorber sans broncher. */
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG, debug_console_parse("dump_dag\n"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG, debug_console_parse("dump_dag\r\n"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG, debug_console_parse("  dump_dag  \r\n"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG, debug_console_parse("\t dump_dag \t"));
}

TEST_CASE("debug_console_parse_ligne_vide_renvoie_EMPTY",
          "[debug_console][parser]")
{
    /* Le moniteur peut envoyer un '\n' nu pour ping — pas une erreur. */
    TEST_ASSERT_EQUAL(DEBUG_CMD_EMPTY, debug_console_parse(""));
    TEST_ASSERT_EQUAL(DEBUG_CMD_EMPTY, debug_console_parse("\n"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_EMPTY, debug_console_parse("   \r\n"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_EMPTY, debug_console_parse(NULL));
}

TEST_CASE("debug_console_parse_commande_inconnue_renvoie_UNKNOWN",
          "[debug_console][parser]")
{
    TEST_ASSERT_EQUAL(DEBUG_CMD_UNKNOWN, debug_console_parse("foo"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_UNKNOWN, debug_console_parse("dump_xyz"));
    /* Une commande connue mais avec un suffixe accole n'est PAS reconnue
     * (le parser exige un token exact). */
    TEST_ASSERT_EQUAL(DEBUG_CMD_UNKNOWN, debug_console_parse("dump_dag2"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_UNKNOWN, debug_console_parse("helpme"));
}

TEST_CASE("debug_console_parse_premier_token_seul",
          "[debug_console][parser]")
{
    /* Si quelqu'un ajoute un argument (par exemple "dump_dag 5"), on
     * accepte la commande et on ignore le reste. Pas d'erreur. */
    TEST_ASSERT_EQUAL(DEBUG_CMD_DUMP_DAG, debug_console_parse("dump_dag 5"));
    TEST_ASSERT_EQUAL(DEBUG_CMD_HELP,     debug_console_parse("help me please"));
}

/* ================================================================
 * Tests du serializer hex
 * ================================================================ */

TEST_CASE("debug_console_hex_encode_serialise_vecteur_connu",
          "[debug_console][hex]")
{
    /* Vecteur trivial : un octet "AB" → "ab" en lowercase. */
    uint8_t src = 0xAB;
    char dst[3];
    int n = debug_console_hex_encode(&src, 1, dst, sizeof(dst));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("ab", dst);
}

TEST_CASE("debug_console_hex_encode_serialise_buffer_long",
          "[debug_console][hex]")
{
    /* 4 octets : 0x00, 0x7F, 0x80, 0xFF — verifie les bords. */
    uint8_t src[] = { 0x00, 0x7F, 0x80, 0xFF };
    char dst[9];
    int n = debug_console_hex_encode(src, sizeof(src), dst, sizeof(dst));
    TEST_ASSERT_EQUAL_INT(8, n);
    TEST_ASSERT_EQUAL_STRING("007f80ff", dst);
}

TEST_CASE("debug_console_hex_encode_refuse_buffer_destination_trop_petit",
          "[debug_console][hex]")
{
    /* Pour 2 octets il faut 5 caracteres (4 hex + '\0'). 4 est trop juste. */
    uint8_t src[] = { 0x12, 0x34 };
    char dst[4];
    int n = debug_console_hex_encode(src, sizeof(src), dst, sizeof(dst));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

TEST_CASE("debug_console_hex_encode_zero_octets_ne_plante_pas",
          "[debug_console][hex]")
{
    /* Cas degenere : 0 octets. On doit obtenir une chaine vide
     * et 0 caracteres ecrits. */
    char dst[4] = "xyz";  /* Pre-remplissage pour verifier l'ecriture du '\0'. */
    uint8_t dummy = 0;
    int n = debug_console_hex_encode(&dummy, 0, dst, sizeof(dst));
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_EQUAL_STRING("", dst);
}

TEST_CASE("debug_console_hex_encode_refuse_pointeurs_nuls",
          "[debug_console][hex]")
{
    char dst[8];
    uint8_t src = 0;
    /* src NULL doit retourner -1 sans crash. */
    TEST_ASSERT_EQUAL_INT(-1, debug_console_hex_encode(NULL, 1, dst, sizeof(dst)));
    /* dst NULL doit retourner -1 sans crash. */
    TEST_ASSERT_EQUAL_INT(-1, debug_console_hex_encode(&src, 1, NULL, 8));
}

#endif /* CONFIG_MESHPAY_DEBUG_CONSOLE */
