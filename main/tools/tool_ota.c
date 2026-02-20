#include "tool_ota.h"
#include "ota/ota_manager.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_ota";

esp_err_t tool_ota_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (!url_item || !cJSON_IsString(url_item) || !url_item->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing or empty 'url' field");
        return ESP_ERR_INVALID_ARG;
    }

    char url[256];
    strlcpy(url, url_item->valuestring, sizeof(url));
    cJSON_Delete(root);

    snprintf(output, output_size,
             "OTA update started from: %s — device will reboot on success.", url);
    ESP_LOGI(TAG, "Triggering OTA from: %s", url);

    ota_update_from_url(url);   /* reboots on success; returns on failure */
    return ESP_OK;
}
