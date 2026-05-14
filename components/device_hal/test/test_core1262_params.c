/**
 * @file test_core1262_params.c
 * @brief Tests unitaires du mapper core1262_map_config().
 *
 * Verifie la traduction hal_lora_config_t -> parametres SX1262 :
 * spreading factor, bande passante, coding rate, LDRO, frequence,
 * puissance, et le rejet des valeurs hors plage.
 */

#include "unity.h"
#include "core1262_params.h"
#include "hal/hal_lora.h"

/* Config de reference du projet : 868.1 MHz, SF9, BW125, CR4/5, 14 dBm. */
static hal_lora_config_t base_config(void)
{
    hal_lora_config_t c = {
        .frequency_hz     = 868100000,
        .spreading_factor = 9,
        .bandwidth        = 0,   /* 0 = 125 kHz */
        .coding_rate      = 1,   /* 1 = 4/5 */
        .tx_power_dbm     = 14,
    };
    return c;
}

TEST_CASE("core1262_params : config de reference mappee correctement", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_SF9,    out.mod.sf);
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_125, out.mod.bw);
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_5, out.mod.cr);
    TEST_ASSERT_EQUAL_UINT8(0,            out.mod.ldro);   /* SF9/BW125 : LDRO off */
    TEST_ASSERT_EQUAL_UINT32(868100000,   out.freq_hz);
    TEST_ASSERT_EQUAL_INT8(14,            out.power_dbm);

    /* Borne basse SF7 : valide. */
    cfg.spreading_factor = 7;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_SF7, out.mod.sf);
}

TEST_CASE("core1262_params : mapping des trois bandes passantes", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    cfg.bandwidth = 0;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_125, out.mod.bw);

    cfg.bandwidth = 1;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_250, out.mod.bw);

    cfg.bandwidth = 2;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_500, out.mod.bw);
}

TEST_CASE("core1262_params : mapping des quatre coding rates", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    cfg.coding_rate = 1;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_5, out.mod.cr);

    cfg.coding_rate = 2;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_6, out.mod.cr);

    cfg.coding_rate = 3;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_7, out.mod.cr);

    cfg.coding_rate = 4;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_8, out.mod.cr);
}

TEST_CASE("core1262_params : LDRO active pour SF11/SF12 en BW125", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    cfg.spreading_factor = 12;
    cfg.bandwidth = 0;  /* 125 kHz */
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(1, out.mod.ldro);

    cfg.spreading_factor = 11;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(1, out.mod.ldro);

    /* SF12 en BW500 : symbole court, LDRO off. */
    cfg.spreading_factor = 12;
    cfg.bandwidth = 2;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(0, out.mod.ldro);

    /* SF12 en BW250 : symbole 16.38 ms, LDRO obligatoire. */
    cfg.spreading_factor = 12;
    cfg.bandwidth = 1;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(1, out.mod.ldro);

    /* SF11 en BW250 : symbole 8.19 ms, LDRO off. */
    cfg.spreading_factor = 11;
    cfg.bandwidth = 1;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(0, out.mod.ldro);
}

TEST_CASE("core1262_params : parametres de paquet par defaut", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT16(8, out.pkt.preamble_len_in_symb);
    TEST_ASSERT_EQUAL(SX126X_LORA_PKT_EXPLICIT, out.pkt.header_type);
    TEST_ASSERT_EQUAL_UINT8(0, out.pkt.pld_len_in_bytes);
    TEST_ASSERT_TRUE(out.pkt.crc_is_on);
    TEST_ASSERT_FALSE(out.pkt.invert_iq_is_on);
}

TEST_CASE("core1262_params : puissance bornee [-9, 22]", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    cfg.tx_power_dbm = -20;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_INT8(-9, out.power_dbm);

    cfg.tx_power_dbm = 30;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_INT8(22, out.power_dbm);

    cfg.tx_power_dbm = -9;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_INT8(-9, out.power_dbm);

    cfg.tx_power_dbm = 22;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_INT8(22, out.power_dbm);
}

TEST_CASE("core1262_params : rejet des valeurs hors plage", "[core1262]")
{
    core1262_radio_params_t out;
    hal_lora_config_t cfg;

    /* SF hors [7,12] */
    cfg = base_config(); cfg.spreading_factor = 6;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));
    cfg = base_config(); cfg.spreading_factor = 13;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));

    /* BW hors [0,2] */
    cfg = base_config(); cfg.bandwidth = 3;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));

    /* CR hors [1,4] */
    cfg = base_config(); cfg.coding_rate = 0;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));
    cfg = base_config(); cfg.coding_rate = 5;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));

    /* Pointeurs NULL */
    cfg = base_config();
    TEST_ASSERT_FALSE(core1262_map_config(NULL, &out));
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, NULL));
}
