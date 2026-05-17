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

static const char *TAG = "ledger_store";

#define LEDGER_PARTITION_LABEL        "storage"
#define LEDGER_SECTOR_SIZE            4096U
#define LEDGER_MAGIC_CHECKPOINT       0x4d50434bU /* MPCK */
#define LEDGER_MAGIC_TX_WINDOW        0x4d505457U /* MPTW */
#define LEDGER_MAGIC_ATTEST_WINDOW    0x4d504157U /* MPAW */
#define LEDGER_FORMAT_VERSION         1U

#define LEDGER_CHECKPOINT_OFFSET      0x00000U
#define LEDGER_CHECKPOINT_REGION_SIZE 0x04000U
#define LEDGER_TX_WINDOW_OFFSET       0x04000U
#define LEDGER_TX_WINDOW_REGION_SIZE  0x10000U
#define LEDGER_TX_WINDOW_MAX          128U
#define LEDGER_ATTEST_WINDOW_OFFSET      0x14000U
#define LEDGER_ATTEST_WINDOW_REGION_SIZE 0x08000U
#define LEDGER_ATTEST_WINDOW_MAX         64U

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

static uint32_t s_checkpoint_generation;
static uint32_t s_tx_window_generation;
static uint32_t s_attestation_window_generation;

static bool header_valid(const ledger_blob_header_t *h, uint32_t magic,
                         uint32_t expected_payload_len);

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

    esp_err_t ret = read_region(LEDGER_CHECKPOINT_OFFSET,
                                blob,
                                sizeof(*blob));
    if (ret == ESP_OK &&
        header_valid(&blob->header, LEDGER_MAGIC_CHECKPOINT,
                     sizeof(checkpoint_t)) &&
        blob->header.checksum ==
            fnv1a32((const uint8_t *)&blob->checkpoint,
                    sizeof(checkpoint_t))) {
        memcpy(checkpoint, &blob->checkpoint, sizeof(*checkpoint));
        s_checkpoint_generation = blob->header.generation;
        ESP_LOGI(TAG, "Checkpoint storage charge (%"PRIu32" comptes, gen=%"PRIu32")",
                 checkpoint->account_count, blob->header.generation);
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

    esp_err_t ret = write_region(LEDGER_CHECKPOINT_OFFSET,
                                 LEDGER_CHECKPOINT_REGION_SIZE,
                                 blob,
                                 sizeof(*blob));
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

    esp_err_t ret = read_region(LEDGER_TX_WINDOW_OFFSET,
                                blob,
                                sizeof(*blob));
    if (ret != ESP_OK ||
        !header_valid(&blob->header, LEDGER_MAGIC_TX_WINDOW,
                      sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX) ||
        blob->header.record_count > LEDGER_TX_WINDOW_MAX ||
        blob->header.checksum !=
            fnv1a32((const uint8_t *)blob->txs,
                    sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX)) {
        ESP_LOGI(TAG, "Fenetre TX storage absente");
        free(blob);
        return ESP_ERR_NOT_FOUND;
    }
    s_tx_window_generation = blob->header.generation;

    uint32_t loaded = 0;
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < blob->header.record_count; i++) {
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

    esp_err_t ret = read_region(LEDGER_TX_WINDOW_OFFSET,
                                blob,
                                sizeof(*blob));
    if (ret != ESP_OK ||
        !header_valid(&blob->header, LEDGER_MAGIC_TX_WINDOW,
                      sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX) ||
        blob->header.record_count > LEDGER_TX_WINDOW_MAX ||
        blob->header.checksum !=
            fnv1a32((const uint8_t *)blob->txs,
                    sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX)) {
        free(blob);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < blob->header.record_count && written < max_count; i++) {
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
    esp_err_t ret = read_region(LEDGER_ATTEST_WINDOW_OFFSET,
                                blob,
                                sizeof(*blob));
    if (ret != ESP_OK ||
        !header_valid(&blob->header, LEDGER_MAGIC_ATTEST_WINDOW,
                      sizeof(comm_msg_attestation_t) * LEDGER_ATTEST_WINDOW_MAX) ||
        blob->header.record_count > LEDGER_ATTEST_WINDOW_MAX ||
        blob->header.checksum !=
            fnv1a32((const uint8_t *)blob->attestations,
                    sizeof(comm_msg_attestation_t) * LEDGER_ATTEST_WINDOW_MAX)) {
        return ESP_ERR_NOT_FOUND;
    }

    s_attestation_window_generation = blob->header.generation;
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

    esp_err_t ret = write_region(LEDGER_ATTEST_WINDOW_OFFSET,
                                 LEDGER_ATTEST_WINDOW_REGION_SIZE,
                                 blob,
                                 sizeof(*blob));
    if (ret == ESP_OK) {
        s_attestation_window_generation = next_generation;
        ESP_LOGI(TAG, "Fenetre attest storage sauvegardee (%"PRIu32" attest)",
                 record_count);
    }
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
    memset(blob, 0xff, sizeof(*blob));

    uint32_t count = 0;
    if (ledger_attestation_window_read_blob(blob) == ESP_OK) {
        count = blob->header.record_count;
        for (uint32_t i = 0; i < count; i++) {
            if (attestation_equal(&blob->attestations[i], att)) {
                free(blob);
                return ESP_OK;
            }
        }
    } else {
        memset(blob, 0xff, sizeof(*blob));
    }

    if (count >= LEDGER_ATTEST_WINDOW_MAX) {
        memmove(&blob->attestations[0], &blob->attestations[1],
                sizeof(comm_msg_attestation_t) * (LEDGER_ATTEST_WINDOW_MAX - 1U));
        count = LEDGER_ATTEST_WINDOW_MAX - 1U;
    }

    memcpy(&blob->attestations[count], att, sizeof(*att));
    count++;

    esp_err_t ret = ledger_attestation_window_write_blob(blob, count);
    free(blob);
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

    esp_err_t ret = ledger_attestation_window_read_blob(blob);
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < blob->header.record_count && written < max_count; i++) {
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

    esp_err_t ret = ledger_attestation_window_read_blob(blob);
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    uint32_t applied = 0;
    for (uint32_t i = 0; i < blob->header.record_count; i++) {
        comm_msg_attestation_t *att = &blob->attestations[i];
        if (comm_msg_verify_attestation(att) != 0) {
            continue;
        }

        const transaction_t *tx = dag_get_by_id(&s_dag, &att->tx_id);
        if (tx == NULL ||
            tx->status == TX_STATUS_CONFIRMED ||
            tx->status == TX_STATUS_CANCELLED ||
            !public_key_equal(&att->attester_key, &tx->to)) {
            continue;
        }

        if (dag_set_status(&s_dag, &att->tx_id, TX_STATUS_CONFIRMED) == ESP_OK) {
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

    uint32_t next_generation = s_tx_window_generation + 1U;
    memset(blob, 0xff, sizeof(*blob));
    memset(blob, 0, sizeof(ledger_blob_header_t));

    uint32_t written = 0;
    ledger_tx_window_blob_t *existing =
        (ledger_tx_window_blob_t *)ledger_alloc(sizeof(*existing));
    if (existing != NULL) {
        if (read_region(LEDGER_TX_WINDOW_OFFSET, existing,
                        sizeof(*existing)) == ESP_OK &&
            header_valid(&existing->header, LEDGER_MAGIC_TX_WINDOW,
                         sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX) &&
            existing->header.record_count <= LEDGER_TX_WINDOW_MAX &&
            existing->header.checksum ==
                fnv1a32((const uint8_t *)existing->txs,
                        sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX)) {
            s_tx_window_generation = existing->header.generation;
            next_generation = s_tx_window_generation + 1U;
            for (uint32_t i = 0; i < existing->header.record_count; i++) {
                const transaction_t *tx = &existing->txs[i];
                if (tx_validate_structure(tx) == ESP_OK &&
                    tx_validate_signature(tx) == ESP_OK) {
                    tx_window_append_or_replace(blob, &written, tx);
                }
            }
        }
        free(existing);
    }

    blob->header.magic = LEDGER_MAGIC_TX_WINDOW;
    blob->header.version = LEDGER_FORMAT_VERSION;
    blob->header.header_size = sizeof(ledger_blob_header_t);
    blob->header.generation = next_generation;
    blob->header.payload_len =
        sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX;

    uint32_t start = 0;
    if (s_dag.count > LEDGER_TX_WINDOW_MAX) {
        start = s_dag.count - LEDGER_TX_WINDOW_MAX;
    }

    if (s_dag.mutex != NULL) {
        xSemaphoreTakeRecursive(s_dag.mutex, portMAX_DELAY);
    }
    for (uint32_t i = start; i < s_dag.count; i++) {
        tx_window_append_or_replace(blob, &written, &s_dag.transactions[i]);
    }
    if (s_dag.mutex != NULL) {
        xSemaphoreGiveRecursive(s_dag.mutex);
    }

    blob->header.record_count = written;
    blob->header.checksum =
        fnv1a32((const uint8_t *)blob->txs,
                sizeof(transaction_t) * LEDGER_TX_WINDOW_MAX);

    esp_err_t ret = write_region(LEDGER_TX_WINDOW_OFFSET,
                                 LEDGER_TX_WINDOW_REGION_SIZE,
                                 blob,
                                 sizeof(*blob));
    if (ret == ESP_OK) {
        s_tx_window_generation = next_generation;
        ESP_LOGI(TAG, "Fenetre TX storage sauvegardee (%"PRIu32" TX, %s)",
                 written, reason ? reason : "?");
    }
    free(blob);
    return ret;
}
