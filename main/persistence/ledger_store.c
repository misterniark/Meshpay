/**
 * @file persistence/ledger_store.c
 * @brief Backend durable checkpoint + fenetre TX sur partition "storage".
 */

#include "ledger_store.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

#include "app_state.h"
#include "dag/dag.h"
#include "persistence/nvs_checkpoint.h"
#include "transaction/tx_validate.h"
#include "tx_lifecycle/tx_lifecycle.h"

static const char *TAG = "ledger_store";

#define LEDGER_PARTITION_LABEL        "storage"
#define LEDGER_SECTOR_SIZE            4096U
#define LEDGER_MAGIC_CHECKPOINT       0x4d50434bU /* MPCK */
#define LEDGER_MAGIC_TX_WINDOW        0x4d505457U /* MPTW */
#define LEDGER_MAGIC_ATTEST_WINDOW    0x4d504157U /* MPAW */
#define LEDGER_FORMAT_VERSION         1U

#define LEDGER_CHECKPOINT_OFFSET      0x00000U
#define LEDGER_CHECKPOINT_REGION_SIZE 0x08000U
#define LEDGER_TX_WINDOW_OFFSET       0x08000U
#define LEDGER_TX_WINDOW_REGION_SIZE  0x20000U
#define LEDGER_TX_WINDOW_MAX          128U
#define LEDGER_ATTEST_WINDOW_OFFSET      0x28000U
#define LEDGER_ATTEST_WINDOW_REGION_SIZE 0x10000U
#define LEDGER_ATTEST_WINDOW_MAX         64U
#define LEDGER_JOURNAL_OFFSET            0x38000U
#define LEDGER_JOURNAL_REGION_SIZE       0x27000U

/* Layout Phase A legacy, single-slot. Used only for one-shot migration. */
#define LEDGER_LEGACY_TX_WINDOW_OFFSET      0x04000U
#define LEDGER_LEGACY_ATTEST_WINDOW_OFFSET  0x14000U

#define LEDGER_MAGIC_JOURNAL_RECORD      0x4d504a4cU /* MPJL */
#define LEDGER_JOURNAL_FORMAT_VERSION    1U
#define LEDGER_JOURNAL_WRITE_ALIGN       16U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint32_t payload_len;
    uint32_t record_count;
    uint32_t checksum;
    uint32_t reserved;
} ledger_blob_header_t;

typedef struct {
    ledger_blob_header_t header;
    checkpoint_t checkpoint;
} ledger_checkpoint_blob_t;

typedef struct {
    ledger_blob_header_t header;
    transaction_t txs[LEDGER_TX_WINDOW_MAX];
} ledger_tx_window_blob_t;

typedef struct {
    ledger_blob_header_t header;
    comm_msg_attestation_t attestations[LEDGER_ATTEST_WINDOW_MAX];
} ledger_attestation_window_blob_t;

typedef enum {
    LEDGER_JOURNAL_RECORD_TX          = 1,
    LEDGER_JOURNAL_RECORD_ATTESTATION = 2,
} ledger_journal_record_type_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint16_t type;
    uint16_t reserved;
    uint32_t payload_len;
    uint32_t payload_checksum;
    uint32_t header_checksum;
} ledger_journal_record_header_t;

_Static_assert(sizeof(ledger_checkpoint_blob_t) <= LEDGER_CHECKPOINT_REGION_SIZE,
               "checkpoint blob too large for ledger checkpoint region");
_Static_assert(sizeof(ledger_tx_window_blob_t) <= LEDGER_TX_WINDOW_REGION_SIZE,
               "tx window blob too large for ledger tx region");
_Static_assert(sizeof(ledger_attestation_window_blob_t) <= LEDGER_ATTEST_WINDOW_REGION_SIZE,
               "attestation window blob too large for ledger attestation region");
_Static_assert(sizeof(ledger_checkpoint_blob_t) <= LEDGER_CHECKPOINT_REGION_SIZE / 2U,
               "checkpoint blob too large for dual-slot ledger checkpoint region");
_Static_assert(sizeof(ledger_tx_window_blob_t) <= LEDGER_TX_WINDOW_REGION_SIZE / 2U,
               "tx window blob too large for dual-slot ledger tx region");
_Static_assert(sizeof(ledger_attestation_window_blob_t) <= LEDGER_ATTEST_WINDOW_REGION_SIZE / 2U,
               "attestation blob too large for dual-slot ledger attestation region");
_Static_assert(sizeof(ledger_journal_record_header_t) == 28U,
               "unexpected ledger journal header size");

static uint32_t s_checkpoint_generation;
static uint32_t s_tx_window_generation;
static uint32_t s_attestation_window_generation;
static uint32_t s_journal_generation;
static uint32_t s_journal_write_offset;
static bool s_journal_scanned;

static bool header_valid(const ledger_blob_header_t *h, uint32_t magic,
                         uint32_t expected_payload_len);
static esp_err_t ledger_tx_window_read_materialized(ledger_tx_window_blob_t *blob,
                                                    uint32_t *out_count);
static esp_err_t ledger_attestation_window_read_materialized(
    ledger_attestation_window_blob_t *blob,
    uint32_t *out_count);

static void *ledger_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = malloc(size);
    }
    return ptr;
}

static const esp_partition_t *ledger_partition(void)
{
    static const esp_partition_t *part;
    if (part == NULL) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_ANY,
                                        LEDGER_PARTITION_LABEL);
        if (part == NULL) {
            ESP_LOGE(TAG, "Partition '%s' introuvable", LEDGER_PARTITION_LABEL);
        }
    }
    return part;
}

static uint32_t fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static esp_err_t read_region(uint32_t offset, void *dst, size_t len)
{
    const esp_partition_t *part = ledger_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_partition_read(part, offset, dst, len);
}

static esp_err_t write_region(uint32_t offset, uint32_t region_size,
                              const void *src, size_t len)
{
    const esp_partition_t *part = ledger_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len > region_size) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *write_buf = (uint8_t *)ledger_alloc(region_size);
    if (write_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(write_buf, 0xff, region_size);
    memcpy(write_buf, src, len);

    esp_err_t ret = esp_partition_erase_range(part, offset, region_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erase storage echoue offset=0x%lx size=0x%lx: %s",
                 (unsigned long)offset, (unsigned long)region_size,
                 esp_err_to_name(ret));
        free(write_buf);
        return ret;
    }

    ret = esp_partition_write(part, offset, write_buf, region_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write storage echoue offset=0x%lx len=%zu: %s",
                 (unsigned long)offset, (size_t)region_size, esp_err_to_name(ret));
    }
    free(write_buf);
    return ret;
}

static esp_err_t read_best_blob(uint32_t offset, uint32_t region_size,
                                uint32_t magic, uint32_t expected_payload_len,
                                void *dst, size_t blob_len,
                                ledger_blob_header_t *out_header)
{
    if (dst == NULL || blob_len < sizeof(ledger_blob_header_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t slot_size = region_size / 2U;
    uint8_t *tmp = (uint8_t *)ledger_alloc(blob_len);
    if (tmp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool found = false;
    uint32_t best_generation = 0;
    ledger_blob_header_t best_header;
    memset(&best_header, 0, sizeof(best_header));

    for (uint32_t slot = 0; slot < 2U; slot++) {
        memset(tmp, 0, blob_len);
        esp_err_t ret = read_region(offset + slot * slot_size, tmp, blob_len);
        if (ret != ESP_OK) {
            continue;
        }

        const ledger_blob_header_t *h = (const ledger_blob_header_t *)tmp;
        const uint8_t *payload = tmp + sizeof(ledger_blob_header_t);
        if (!header_valid(h, magic, expected_payload_len) ||
            h->checksum != fnv1a32(payload, expected_payload_len)) {
            continue;
        }

        if (!found || h->generation > best_generation) {
            memcpy(dst, tmp, blob_len);
            best_header = *h;
            best_generation = h->generation;
            found = true;
        }
    }

    free(tmp);
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    if (out_header != NULL) {
        *out_header = best_header;
    }
    return ESP_OK;
}

static esp_err_t read_legacy_blob(uint32_t offset,
                                  uint32_t magic,
                                  uint32_t expected_payload_len,
                                  void *dst,
                                  size_t blob_len,
                                  ledger_blob_header_t *out_header)
{
    if (dst == NULL || blob_len < sizeof(ledger_blob_header_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = read_region(offset, dst, blob_len);
    if (ret != ESP_OK) {
        return ret;
    }

    const ledger_blob_header_t *h = (const ledger_blob_header_t *)dst;
    const uint8_t *payload = (const uint8_t *)dst + sizeof(ledger_blob_header_t);
    if (!header_valid(h, magic, expected_payload_len) ||
        h->checksum != fnv1a32(payload, expected_payload_len)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (out_header != NULL) {
        *out_header = *h;
    }
    return ESP_OK;
}

static esp_err_t write_blob_next(uint32_t offset, uint32_t region_size,
                                 const void *src, size_t blob_len,
                                 uint32_t generation)
{
    const uint32_t slot_size = region_size / 2U;
    const uint32_t slot = generation % 2U;
    return write_region(offset + slot * slot_size, slot_size, src, blob_len);
}

static bool header_valid(const ledger_blob_header_t *h, uint32_t magic,
                         uint32_t expected_payload_len)
{
    return h != NULL &&
           h->magic == magic &&
           h->version == LEDGER_FORMAT_VERSION &&
           h->header_size == sizeof(ledger_blob_header_t) &&
           h->payload_len == expected_payload_len;
}

static uint32_t align_up_u32(uint32_t value, uint32_t align)
{
    return (value + align - 1U) & ~(align - 1U);
}

static uint32_t journal_header_checksum(const ledger_journal_record_header_t *h)
{
    ledger_journal_record_header_t tmp = *h;
    tmp.header_checksum = 0;
    return fnv1a32((const uint8_t *)&tmp, sizeof(tmp));
}

static bool journal_header_valid(const ledger_journal_record_header_t *h)
{
    return h != NULL &&
           h->magic == LEDGER_MAGIC_JOURNAL_RECORD &&
           h->version == LEDGER_JOURNAL_FORMAT_VERSION &&
           h->header_size == sizeof(ledger_journal_record_header_t) &&
           h->payload_len > 0 &&
           h->header_checksum == journal_header_checksum(h);
}

static bool journal_type_len_valid(uint16_t type, uint32_t payload_len)
{
    switch ((ledger_journal_record_type_t)type) {
    case LEDGER_JOURNAL_RECORD_TX:
        return payload_len == sizeof(transaction_t);
    case LEDGER_JOURNAL_RECORD_ATTESTATION:
        return payload_len == sizeof(comm_msg_attestation_t);
    default:
        return false;
    }
}

static esp_err_t journal_scan_state(void)
{
    if (s_journal_scanned) {
        return ESP_OK;
    }

    const esp_partition_t *part = ledger_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t offset = 0;
    uint32_t best_generation = 0;
    while (offset + sizeof(ledger_journal_record_header_t) <=
           LEDGER_JOURNAL_REGION_SIZE) {
        ledger_journal_record_header_t h;
        esp_err_t ret = read_region(LEDGER_JOURNAL_OFFSET + offset,
                                    &h, sizeof(h));
        if (ret != ESP_OK) {
            return ret;
        }

        if (h.magic == 0xffffffffU) {
            break;
        }
        if (!journal_header_valid(&h) ||
            !journal_type_len_valid(h.type, h.payload_len)) {
            /*
             * Sur partition chiffrée, une zone effacée ne se relit pas
             * forcément comme 0xff après déchiffrement. Un header invalide
             * marque donc simplement la fin du journal exploitable ; le cas
             * couvre aussi un record interrompu pendant une coupure.
             */
            ESP_LOGD(TAG, "Fin journal ledger a 0x%lx",
                     (unsigned long)(LEDGER_JOURNAL_OFFSET + offset));
            break;
        }

        uint32_t total = align_up_u32(sizeof(h) + h.payload_len,
                                      LEDGER_JOURNAL_WRITE_ALIGN);
        if (total == 0 ||
            offset + total > LEDGER_JOURNAL_REGION_SIZE) {
            break;
        }

        uint8_t payload_buf[sizeof(transaction_t)];
        uint8_t *payload = payload_buf;
        if (h.payload_len > sizeof(payload_buf)) {
            payload = (uint8_t *)ledger_alloc(h.payload_len);
            if (payload == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }
        ret = read_region(LEDGER_JOURNAL_OFFSET + offset + sizeof(h),
                          payload, h.payload_len);
        bool checksum_ok = (ret == ESP_OK &&
                            h.payload_checksum == fnv1a32(payload, h.payload_len));
        if (payload != payload_buf) {
            free(payload);
        }
        if (!checksum_ok) {
            ESP_LOGD(TAG, "Fin journal ledger payload invalide a 0x%lx",
                     (unsigned long)(LEDGER_JOURNAL_OFFSET + offset));
            break;
        }

        if (h.generation > best_generation) {
            best_generation = h.generation;
        }
        offset += total;
    }

    s_journal_generation = best_generation;
    s_journal_write_offset = offset;
    s_journal_scanned = true;
    ESP_LOGI(TAG, "Journal ledger scanne (offset=0x%lx, gen=%"PRIu32")",
             (unsigned long)s_journal_write_offset, s_journal_generation);
    return ESP_OK;
}

static esp_err_t journal_erase(void)
{
    const esp_partition_t *part = ledger_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = esp_partition_erase_range(part,
                                              LEDGER_JOURNAL_OFFSET,
                                              LEDGER_JOURNAL_REGION_SIZE);
    if (ret == ESP_OK) {
        s_journal_generation = 0;
        s_journal_write_offset = 0;
        s_journal_scanned = true;
        ESP_LOGI(TAG, "Journal ledger compacte/efface");
    }
    return ret;
}

static esp_err_t journal_append(uint16_t type, const void *payload,
                                uint32_t payload_len)
{
    if (payload == NULL || !journal_type_len_valid(type, payload_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = journal_scan_state();
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t raw_len = sizeof(ledger_journal_record_header_t) + payload_len;
    uint32_t total_len = align_up_u32(raw_len, LEDGER_JOURNAL_WRITE_ALIGN);
    if (s_journal_write_offset + total_len > LEDGER_JOURNAL_REGION_SIZE) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t stack_buf[512];
    uint8_t *buf = stack_buf;
    if (total_len > sizeof(stack_buf)) {
        buf = (uint8_t *)ledger_alloc(total_len);
        if (buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(buf, 0xff, total_len);
    ledger_journal_record_header_t *h = (ledger_journal_record_header_t *)buf;
    h->magic = LEDGER_MAGIC_JOURNAL_RECORD;
    h->version = LEDGER_JOURNAL_FORMAT_VERSION;
    h->header_size = sizeof(*h);
    h->generation = s_journal_generation + 1U;
    h->type = type;
    h->reserved = 0;
    h->payload_len = payload_len;
    h->payload_checksum = fnv1a32((const uint8_t *)payload, payload_len);
    h->header_checksum = journal_header_checksum(h);
    memcpy(buf + sizeof(*h), payload, payload_len);

    const esp_partition_t *part = ledger_partition();
    if (part == NULL) {
        if (buf != stack_buf) free(buf);
        return ESP_ERR_NOT_FOUND;
    }

    ret = esp_partition_write(part,
                              LEDGER_JOURNAL_OFFSET + s_journal_write_offset,
                              buf,
                              total_len);
    if (ret == ESP_OK) {
        s_journal_generation = h->generation;
        s_journal_write_offset += total_len;
    } else {
        ESP_LOGE(TAG, "Append journal ledger echoue: %s", esp_err_to_name(ret));
    }

    if (buf != stack_buf) {
        free(buf);
    }
    return ret;
}

esp_err_t ledger_checkpoint_load(checkpoint_t *checkpoint, void *ctx)
{
    (void)ctx;
    if (checkpoint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ledger_checkpoint_blob_t *blob =
        (ledger_checkpoint_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(blob, 0, sizeof(*blob));

    ledger_blob_header_t header;
    esp_err_t ret = read_best_blob(LEDGER_CHECKPOINT_OFFSET,
                                   LEDGER_CHECKPOINT_REGION_SIZE,
                                   LEDGER_MAGIC_CHECKPOINT,
                                   sizeof(checkpoint_t),
                                   blob,
                                   sizeof(*blob),
                                   &header);
    if (ret == ESP_OK) {
        memcpy(checkpoint, &blob->checkpoint, sizeof(*checkpoint));
        s_checkpoint_generation = header.generation;
        ESP_LOGI(TAG, "Checkpoint storage charge (%"PRIu32" comptes, gen=%"PRIu32")",
                 checkpoint->account_count, header.generation);
        free(blob);
        return ESP_OK;
    }
    free(blob);

    /*
     * Migration douce : si le nouveau backend est vide, recuperer l'ancien
     * checkpoint NVS puis le copier dans la partition storage.
     */
    ret = nvs_checkpoint_load(checkpoint, NULL);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "Checkpoint migre depuis NVS vers storage");
        (void)ledger_checkpoint_save(checkpoint, NULL);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t ledger_checkpoint_save(const checkpoint_t *checkpoint, void *ctx)
{
    (void)ctx;
    if (checkpoint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ledger_checkpoint_blob_t *blob =
        (ledger_checkpoint_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t next_generation = s_checkpoint_generation + 1U;
    memset(blob, 0xff, sizeof(*blob));
    memset(blob, 0, sizeof(ledger_blob_header_t));

    blob->header.magic = LEDGER_MAGIC_CHECKPOINT;
    blob->header.version = LEDGER_FORMAT_VERSION;
    blob->header.header_size = sizeof(ledger_blob_header_t);
    blob->header.generation = next_generation;
    blob->header.payload_len = sizeof(checkpoint_t);
    blob->header.record_count = checkpoint->account_count;
    memcpy(&blob->checkpoint, checkpoint, sizeof(*checkpoint));
    blob->header.checksum =
        fnv1a32((const uint8_t *)&blob->checkpoint,
                sizeof(checkpoint_t));

    esp_err_t ret = write_blob_next(LEDGER_CHECKPOINT_OFFSET,
                                    LEDGER_CHECKPOINT_REGION_SIZE,
                                    blob,
                                    sizeof(*blob),
                                    next_generation);
    if (ret == ESP_OK) {
        s_checkpoint_generation = next_generation;
        ESP_LOGI(TAG, "Checkpoint storage sauvegarde (%"PRIu32" comptes, gen=%"PRIu32")",
                 checkpoint->account_count, next_generation);
    }
    free(blob);
    return ret;
}

esp_err_t ledger_tx_window_load_into_dag(void)
{
    ledger_tx_window_blob_t *blob =
        (ledger_tx_window_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(blob, 0, sizeof(*blob));

    uint32_t record_count = 0;
    esp_err_t ret = ledger_tx_window_read_materialized(blob, &record_count);
    if (ret != ESP_OK || record_count > LEDGER_TX_WINDOW_MAX) {
        ESP_LOGI(TAG, "Fenetre TX storage absente");
        free(blob);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t loaded = 0;
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < record_count; i++) {
        const transaction_t *tx = &blob->txs[i];

        if (tx_validate_structure(tx) != ESP_OK ||
            tx_validate_signature(tx) != ESP_OK) {
            skipped++;
            continue;
        }

        /*
         * Ne pas recharger les TX deja consolidees dans le checkpoint :
         * elles serviraient deux fois au calcul de solde. Les preuves
         * reseau speciales (seed test) restent gerees separement.
         */
        if (s_checkpoint.timestamp > 0 &&
            tx->timestamp <= s_checkpoint.timestamp) {
            skipped++;
            continue;
        }

        ret = dag_insert(&s_dag, tx);
        if (ret == ESP_OK) {
            loaded++;
        } else {
            skipped++;
        }
    }

    ESP_LOGI(TAG, "Fenetre TX storage chargee (%"PRIu32" TX, %"PRIu32" ignorees)",
             loaded, skipped);
    free(blob);
    return ESP_OK;
}

esp_err_t ledger_tx_window_read_recent(transaction_t *out_txs,
                                       uint32_t max_count,
                                       uint32_t *out_count)
{
    if (out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    if (out_txs == NULL || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ledger_tx_window_blob_t *blob =
        (ledger_tx_window_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(blob, 0, sizeof(*blob));

    uint32_t record_count = 0;
    esp_err_t ret = ledger_tx_window_read_materialized(blob, &record_count);
    if (ret != ESP_OK || record_count > LEDGER_TX_WINDOW_MAX) {
        free(blob);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < record_count && written < max_count; i++) {
        const transaction_t *tx = &blob->txs[i];
        if (tx_validate_structure(tx) != ESP_OK ||
            tx_validate_signature(tx) != ESP_OK) {
            continue;
        }
        memcpy(&out_txs[written], tx, sizeof(transaction_t));
        written++;
    }

    *out_count = written;
    free(blob);
    return ESP_OK;
}

static esp_err_t ledger_attestation_window_read_blob(ledger_attestation_window_blob_t *blob)
{
    if (blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(blob, 0, sizeof(*blob));
    ledger_blob_header_t header;
    esp_err_t ret = read_best_blob(LEDGER_ATTEST_WINDOW_OFFSET,
                                   LEDGER_ATTEST_WINDOW_REGION_SIZE,
                                   LEDGER_MAGIC_ATTEST_WINDOW,
                                   sizeof(comm_msg_attestation_t) * LEDGER_ATTEST_WINDOW_MAX,
                                   blob,
                                   sizeof(*blob),
                                   &header);
    if (ret != ESP_OK) {
        ret = read_legacy_blob(LEDGER_LEGACY_ATTEST_WINDOW_OFFSET,
                               LEDGER_MAGIC_ATTEST_WINDOW,
                               sizeof(comm_msg_attestation_t) * LEDGER_ATTEST_WINDOW_MAX,
                               blob,
                               sizeof(*blob),
                               &header);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Fenetre attest storage migree depuis layout legacy");
        }
    }
    if (ret != ESP_OK || header.record_count > LEDGER_ATTEST_WINDOW_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    blob->header = header;
    s_attestation_window_generation = header.generation;
    return ESP_OK;
}

static bool attestation_equal(const comm_msg_attestation_t *a,
                              const comm_msg_attestation_t *b)
{
    return a != NULL && b != NULL &&
           memcmp(a->attester_key.bytes, b->attester_key.bytes,
                  CRYPTO_PUBLIC_KEY_SIZE) == 0 &&
           memcmp(a->tx_id.bytes, b->tx_id.bytes, CRYPTO_HASH_SIZE) == 0;
}

static bool tx_id_equal(const transaction_t *tx, const hash_t *id)
{
    return tx != NULL && id != NULL &&
           memcmp(tx->id.bytes, id->bytes, CRYPTO_HASH_SIZE) == 0;
}

static void tx_window_append_or_replace(ledger_tx_window_blob_t *blob,
                                        uint32_t *count,
                                        const transaction_t *tx)
{
    if (blob == NULL || count == NULL || tx == NULL) {
        return;
    }

    for (uint32_t i = 0; i < *count; i++) {
        if (tx_id_equal(&blob->txs[i], &tx->id)) {
            memcpy(&blob->txs[i], tx, sizeof(*tx));
            return;
        }
    }

    if (*count >= LEDGER_TX_WINDOW_MAX) {
        memmove(&blob->txs[0], &blob->txs[1],
                sizeof(transaction_t) * (LEDGER_TX_WINDOW_MAX - 1U));
        *count = LEDGER_TX_WINDOW_MAX - 1U;
    }

    memcpy(&blob->txs[*count], tx, sizeof(*tx));
    (*count)++;
}

static void attestation_window_append_unique(ledger_attestation_window_blob_t *blob,
                                             uint32_t *count,
                                             const comm_msg_attestation_t *att)
{
    if (blob == NULL || count == NULL || att == NULL) {
        return;
    }

    for (uint32_t i = 0; i < *count; i++) {
        if (attestation_equal(&blob->attestations[i], att)) {
            return;
        }
    }

    if (*count >= LEDGER_ATTEST_WINDOW_MAX) {
        memmove(&blob->attestations[0], &blob->attestations[1],
                sizeof(comm_msg_attestation_t) * (LEDGER_ATTEST_WINDOW_MAX - 1U));
        *count = LEDGER_ATTEST_WINDOW_MAX - 1U;
    }

    memcpy(&blob->attestations[*count], att, sizeof(*att));
    (*count)++;
}

static esp_err_t journal_replay_windows(ledger_tx_window_blob_t *tx_blob,
                                        uint32_t *tx_count,
                                        ledger_attestation_window_blob_t *att_blob,
                                        uint32_t *att_count)
{
    esp_err_t ret = journal_scan_state();
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t offset = 0;
    while (offset + sizeof(ledger_journal_record_header_t) <=
           s_journal_write_offset) {
        ledger_journal_record_header_t h;
        ret = read_region(LEDGER_JOURNAL_OFFSET + offset, &h, sizeof(h));
        if (ret != ESP_OK || !journal_header_valid(&h) ||
            !journal_type_len_valid(h.type, h.payload_len)) {
            break;
        }

        uint32_t total = align_up_u32(sizeof(h) + h.payload_len,
                                      LEDGER_JOURNAL_WRITE_ALIGN);
        if (offset + total > s_journal_write_offset) {
            break;
        }

        if (h.type == LEDGER_JOURNAL_RECORD_TX && tx_blob && tx_count) {
            transaction_t tx;
            ret = read_region(LEDGER_JOURNAL_OFFSET + offset + sizeof(h),
                              &tx, sizeof(tx));
            if (ret != ESP_OK ||
                h.payload_checksum != fnv1a32((const uint8_t *)&tx, sizeof(tx))) {
                break;
            }
            if (tx_validate_structure(&tx) == ESP_OK &&
                tx_validate_signature(&tx) == ESP_OK) {
                tx_window_append_or_replace(tx_blob, tx_count, &tx);
            }
        } else if (h.type == LEDGER_JOURNAL_RECORD_ATTESTATION &&
                   att_blob && att_count) {
            comm_msg_attestation_t att;
            ret = read_region(LEDGER_JOURNAL_OFFSET + offset + sizeof(h),
                              &att, sizeof(att));
            if (ret != ESP_OK ||
                h.payload_checksum != fnv1a32((const uint8_t *)&att, sizeof(att))) {
                break;
            }
            if (comm_msg_verify_attestation(&att) == 0) {
                attestation_window_append_unique(att_blob, att_count, &att);
            }
        }

        offset += total;
    }

    return ESP_OK;
}

static esp_err_t ledger_tx_window_read_materialized(ledger_tx_window_blob_t *blob,
                                                   uint32_t *out_count)
{
    if (blob == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(blob, 0xff, sizeof(*blob));
    memset(&blob->header, 0, sizeof(blob->header));
    *out_count = 0;

    ledger_blob_header_t header;
    esp_err_t ret = read_best_blob(LEDGER_TX_WINDOW_OFFSET,
                                   LEDGER_TX_WINDOW_REGION_SIZE,
                                   LEDGER_MAGIC_TX_WINDOW,
                                   sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX,
                                   blob,
                                   sizeof(*blob),
                                   &header);
    if (ret != ESP_OK) {
        ret = read_legacy_blob(LEDGER_LEGACY_TX_WINDOW_OFFSET,
                               LEDGER_MAGIC_TX_WINDOW,
                               sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX,
                               blob,
                               sizeof(*blob),
                               &header);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Fenetre TX storage migree depuis layout legacy");
        }
    }

    if (ret == ESP_OK && header.record_count <= LEDGER_TX_WINDOW_MAX) {
        s_tx_window_generation = header.generation;
        for (uint32_t i = 0; i < header.record_count; i++) {
            const transaction_t *tx = &blob->txs[i];
            if (tx_validate_structure(tx) == ESP_OK &&
                tx_validate_signature(tx) == ESP_OK) {
                tx_window_append_or_replace(blob, out_count, tx);
            }
        }
    } else {
        memset(blob, 0xff, sizeof(*blob));
        memset(&blob->header, 0, sizeof(blob->header));
    }

    ret = journal_replay_windows(blob, out_count, NULL, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    blob->header.record_count = *out_count;
    return (*out_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t ledger_attestation_window_read_materialized(
    ledger_attestation_window_blob_t *blob,
    uint32_t *out_count)
{
    if (blob == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(blob, 0xff, sizeof(*blob));
    memset(&blob->header, 0, sizeof(blob->header));
    *out_count = 0;

    ledger_attestation_window_blob_t *snapshot =
        (ledger_attestation_window_blob_t *)ledger_alloc(sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (ledger_attestation_window_read_blob(snapshot) == ESP_OK &&
        snapshot->header.record_count <= LEDGER_ATTEST_WINDOW_MAX) {
        for (uint32_t i = 0; i < snapshot->header.record_count; i++) {
            if (comm_msg_verify_attestation(&snapshot->attestations[i]) == 0) {
                attestation_window_append_unique(blob, out_count,
                                                 &snapshot->attestations[i]);
            }
        }
    }
    free(snapshot);

    esp_err_t ret = journal_replay_windows(NULL, NULL, blob, out_count);
    if (ret != ESP_OK) {
        return ret;
    }
    blob->header.record_count = *out_count;
    return (*out_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t ledger_tx_window_write_blob(ledger_tx_window_blob_t *blob,
                                             uint32_t record_count,
                                             const char *reason)
{
    if (blob == NULL || record_count > LEDGER_TX_WINDOW_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t next_generation = s_tx_window_generation + 1U;
    memset(&blob->header, 0, sizeof(blob->header));
    blob->header.magic = LEDGER_MAGIC_TX_WINDOW;
    blob->header.version = LEDGER_FORMAT_VERSION;
    blob->header.header_size = sizeof(ledger_blob_header_t);
    blob->header.generation = next_generation;
    blob->header.payload_len = sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX;
    blob->header.record_count = record_count;
    blob->header.checksum =
        fnv1a32((const uint8_t *)blob->txs,
                sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX);

    esp_err_t ret = write_blob_next(LEDGER_TX_WINDOW_OFFSET,
                                    LEDGER_TX_WINDOW_REGION_SIZE,
                                    blob,
                                    sizeof(*blob),
                                    next_generation);
    if (ret == ESP_OK) {
        s_tx_window_generation = next_generation;
        ESP_LOGI(TAG, "Fenetre TX snapshot sauvegardee (%"PRIu32" TX, %s)",
                 record_count, reason ? reason : "?");
    }
    return ret;
}

static esp_err_t ledger_attestation_window_write_blob(
    ledger_attestation_window_blob_t *blob,
    uint32_t record_count)
{
    if (blob == NULL || record_count > LEDGER_ATTEST_WINDOW_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t next_generation = s_attestation_window_generation + 1U;
    memset(&blob->header, 0, sizeof(blob->header));
    blob->header.magic = LEDGER_MAGIC_ATTEST_WINDOW;
    blob->header.version = LEDGER_FORMAT_VERSION;
    blob->header.header_size = sizeof(ledger_blob_header_t);
    blob->header.generation = next_generation;
    blob->header.payload_len =
        sizeof(comm_msg_attestation_t) * LEDGER_ATTEST_WINDOW_MAX;
    blob->header.record_count = record_count;
    blob->header.checksum =
        fnv1a32((const uint8_t *)blob->attestations,
                sizeof(comm_msg_attestation_t) * LEDGER_ATTEST_WINDOW_MAX);

    esp_err_t ret = write_blob_next(LEDGER_ATTEST_WINDOW_OFFSET,
                                    LEDGER_ATTEST_WINDOW_REGION_SIZE,
                                    blob,
                                    sizeof(*blob),
                                    next_generation);
    if (ret == ESP_OK) {
        s_attestation_window_generation = next_generation;
        ESP_LOGI(TAG, "Fenetre attest storage sauvegardee (%"PRIu32" attest)",
                 record_count);
    }
    return ret;
}

static esp_err_t ledger_journal_compact(const char *reason)
{
    ledger_tx_window_blob_t *tx_blob =
        (ledger_tx_window_blob_t *)ledger_alloc(sizeof(*tx_blob));
    ledger_attestation_window_blob_t *att_blob =
        (ledger_attestation_window_blob_t *)ledger_alloc(sizeof(*att_blob));
    if (tx_blob == NULL || att_blob == NULL) {
        free(tx_blob);
        free(att_blob);
        return ESP_ERR_NO_MEM;
    }

    uint32_t tx_count = 0;
    uint32_t att_count = 0;
    esp_err_t tx_ret = ledger_tx_window_read_materialized(tx_blob, &tx_count);
    esp_err_t att_ret = ledger_attestation_window_read_materialized(att_blob,
                                                                   &att_count);
    if (tx_ret != ESP_OK) {
        memset(tx_blob, 0xff, sizeof(*tx_blob));
        memset(&tx_blob->header, 0, sizeof(tx_blob->header));
        tx_count = 0;
    }
    if (att_ret != ESP_OK) {
        memset(att_blob, 0xff, sizeof(*att_blob));
        memset(&att_blob->header, 0, sizeof(att_blob->header));
        att_count = 0;
    }

    esp_err_t ret = ledger_tx_window_write_blob(tx_blob, tx_count,
                                                reason ? reason : "journal_compact");
    if (ret == ESP_OK) {
        ret = ledger_attestation_window_write_blob(att_blob, att_count);
    }
    if (ret == ESP_OK) {
        ret = journal_erase();
    }

    free(tx_blob);
    free(att_blob);
    return ret;
}

esp_err_t ledger_attestation_window_add(const comm_msg_attestation_t *att)
{
    if (att == NULL || comm_msg_verify_attestation(att) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ledger_attestation_window_blob_t *blob =
        (ledger_attestation_window_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t count = 0;
    if (ledger_attestation_window_read_materialized(blob, &count) == ESP_OK) {
        for (uint32_t i = 0; i < count; i++) {
            if (attestation_equal(&blob->attestations[i], att)) {
                free(blob);
                return ESP_OK;
            }
        }
    }
    free(blob);

    esp_err_t ret = journal_append(LEDGER_JOURNAL_RECORD_ATTESTATION,
                                   att, sizeof(*att));
    if (ret == ESP_ERR_NO_MEM) {
        ret = ledger_journal_compact("attestation_journal_full");
        if (ret == ESP_OK) {
            ret = journal_append(LEDGER_JOURNAL_RECORD_ATTESTATION,
                                 att, sizeof(*att));
        }
    }
    return ret;
}

esp_err_t ledger_attestation_window_read_recent(comm_msg_attestation_t *out,
                                                uint32_t max_count,
                                                uint32_t *out_count)
{
    if (out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (out == NULL || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ledger_attestation_window_blob_t *blob =
        (ledger_attestation_window_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t record_count = 0;
    esp_err_t ret = ledger_attestation_window_read_materialized(blob,
                                                               &record_count);
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < record_count && written < max_count; i++) {
        if (comm_msg_verify_attestation(&blob->attestations[i]) != 0) {
            continue;
        }
        memcpy(&out[written], &blob->attestations[i], sizeof(out[written]));
        written++;
    }

    *out_count = written;
    free(blob);
    return ESP_OK;
}

esp_err_t ledger_attestation_apply_to_dag(uint32_t *out_applied)
{
    if (out_applied != NULL) {
        *out_applied = 0;
    }

    ledger_attestation_window_blob_t *blob =
        (ledger_attestation_window_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t record_count = 0;
    esp_err_t ret = ledger_attestation_window_read_materialized(blob,
                                                               &record_count);
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    uint32_t applied = 0;
    for (uint32_t i = 0; i < record_count; i++) {
        comm_msg_attestation_t *att = &blob->attestations[i];
        if (comm_msg_verify_attestation(att) != 0) {
            continue;
        }

        const transaction_t *tx = dag_get_by_id(&s_dag, &att->tx_id);
        if (tx == NULL ||
            tx->status == TX_STATUS_CONFIRMED ||
            tx->status == TX_STATUS_CANCELLED) {
            continue;
        }

        if (tx_lifecycle_confirm_by_attestation(
                &s_dag, att,
                TX_LIFECYCLE_CONFIRM_REPLAYED_ATTESTATION) == ESP_OK) {
            applied++;
        }
    }

    if (out_applied != NULL) {
        *out_applied = applied;
    }
    if (applied > 0) {
        ESP_LOGI(TAG, "Attestations rejouees sur DAG (%"PRIu32")", applied);
    }
    free(blob);
    return ESP_OK;
}

esp_err_t ledger_tx_window_save_from_dag(const char *reason)
{
    ledger_tx_window_blob_t *blob =
        (ledger_tx_window_blob_t *)ledger_alloc(sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t written = 0;
    esp_err_t ret = ledger_tx_window_read_materialized(blob, &written);
    if (ret != ESP_OK) {
        memset(blob, 0xff, sizeof(*blob));
        memset(&blob->header, 0, sizeof(blob->header));
        written = 0;
    }

    uint32_t start = 0;
    if (s_dag.count > LEDGER_TX_WINDOW_MAX) {
        start = s_dag.count - LEDGER_TX_WINDOW_MAX;
    }

    transaction_t candidates[LEDGER_TX_WINDOW_MAX];
    uint32_t candidate_count = 0;
    if (s_dag.mutex != NULL) {
        xSemaphoreTakeRecursive(s_dag.mutex, portMAX_DELAY);
    }
    for (uint32_t i = start; i < s_dag.count &&
                         candidate_count < LEDGER_TX_WINDOW_MAX; i++) {
        candidates[candidate_count++] = s_dag.transactions[i];
    }
    if (s_dag.mutex != NULL) {
        xSemaphoreGiveRecursive(s_dag.mutex);
    }

    uint32_t appended = 0;
    for (uint32_t i = 0; i < candidate_count; i++) {
        const transaction_t *tx = &candidates[i];
        bool changed = true;
        for (uint32_t j = 0; j < written; j++) {
            if (tx_id_equal(&blob->txs[j], &tx->id)) {
                changed = (memcmp(&blob->txs[j], tx, sizeof(*tx)) != 0);
                break;
            }
        }
        if (!changed) {
            continue;
        }

        ret = journal_append(LEDGER_JOURNAL_RECORD_TX, tx, sizeof(*tx));
        if (ret == ESP_ERR_NO_MEM) {
            ret = ledger_journal_compact("tx_journal_full");
            if (ret == ESP_OK) {
                ret = journal_append(LEDGER_JOURNAL_RECORD_TX, tx, sizeof(*tx));
            }
        }
        if (ret != ESP_OK) {
            free(blob);
            return ret;
        }
        tx_window_append_or_replace(blob, &written, tx);
        appended++;
    }

    if (appended > 0) {
        ESP_LOGI(TAG, "Fenetre TX journalisee (%"PRIu32" changements, %s)",
                 appended, reason ? reason : "?");
    }

    free(blob);
    return ESP_OK;
}
