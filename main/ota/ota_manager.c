#include "ota_manager.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_timer.h"

static const char *TAG = "ota";

static void ota_restart_cb(void *arg)
{
    esp_restart();
}

esp_err_t ota_update_from_url(const char *url)
{
    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting in 5 seconds...");
        /* Deferred restart: give the agent loop time to send its final
         * response before the device reboots. */
        esp_timer_handle_t t;
        const esp_timer_create_args_t args = {
            .callback = ota_restart_cb,
            .name     = "ota_restart",
        };
        if (esp_timer_create(&args, &t) == ESP_OK) {
            esp_timer_start_once(t, 5ULL * 1000 * 1000);  /* 5 seconds */
        } else {
            esp_restart();  /* fallback: immediate restart */
        }
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    return ret;
}
