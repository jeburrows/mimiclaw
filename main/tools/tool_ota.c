#include "tool_ota.h"
#include "ota/ota_manager.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_ota";

esp_err_t tool_ota_execute(const char *input_json, char *output, size_t output_size)
{
    char url[256] = MIMI_SECRET_OTA_URL;

    cJSON *root = cJSON_Parse(input_json);
    if (root) {
        cJSON *url_item = cJSON_GetObjectItem(root, "url");
        if (url_item && cJSON_IsString(url_item) && url_item->valuestring[0]) {
            strlcpy(url, url_item->valuestring, sizeof(url));
        }
        cJSON_Delete(root);
    }

    if (url[0] == '\0') {
        snprintf(output, output_size, "Error: no OTA URL configured. Set MIMI_SECRET_OTA_URL in mimi_secrets.h");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Triggering OTA from: %s", url);

    esp_err_t ret = ota_update_from_url(url);

    if (ret == ESP_OK) {
        snprintf(output, output_size,
                 "OTA successful from %s — device will reboot in ~5 seconds. "
                 "I will be back online shortly on the new firmware.", url);
    } else {
        snprintf(output, output_size,
                 "OTA FAILED from %s (%s). Device is still running the current firmware.",
                 url, esp_err_to_name(ret));
    }

    return ret;
}
