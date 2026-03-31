#include "ota_manager.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "ota";

#define OTA_TASK_STACK_SIZE  (12 * 1024)
#define OTA_TASK_TIMEOUT_MS  (180 * 1000)

static void ota_restart_cb(void *arg)
{
    esp_restart();
}

typedef struct {
    const char      *url;
    esp_err_t        result;
    SemaphoreHandle_t done;
} ota_run_args_t;

static void ota_run_task(void *pvarg)
{
    ota_run_args_t *a = (ota_run_args_t *)pvarg;

    esp_http_client_config_t config = {
        .url              = a->url,
        .timeout_ms       = 120000,
        .buffer_size      = 4096,
        .buffer_size_tx   = 4096,   /* needed for TLS handshake to redirect target */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    a->result = esp_https_ota(&ota_config);
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

esp_err_t ota_update_from_url(const char *url)
{
    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_ERR_NO_MEM;
    }

    ota_run_args_t args = { .url = url, .result = ESP_FAIL, .done = done };

    /* Run OTA in its own task so it has a clean, adequately-sized stack.
     * Running esp_https_ota() directly inside the agent task risks stack
     * overflow because the agent frame + TLS + flash writes exceed 24 KB. */
    if (xTaskCreate(ota_run_task, "ota_run", OTA_TASK_STACK_SIZE, &args, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        vSemaphoreDelete(done);
        return ESP_ERR_NO_MEM;
    }

    /* Block until the OTA task signals completion (or timeout) */
    if (xSemaphoreTake(done, pdMS_TO_TICKS(OTA_TASK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "OTA timed out after %d seconds", OTA_TASK_TIMEOUT_MS / 1000);
        vSemaphoreDelete(done);
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(done);

    if (args.result == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting in 5 seconds...");
        esp_timer_handle_t t;
        const esp_timer_create_args_t targs = {
            .callback = ota_restart_cb,
            .name     = "ota_restart",
        };
        if (esp_timer_create(&targs, &t) == ESP_OK) {
            esp_timer_start_once(t, 5ULL * 1000 * 1000);
        } else {
            esp_restart();
        }
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(args.result));
    }

    return args.result;
}
