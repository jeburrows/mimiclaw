#include "tool_http_get.h"

#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "tool_http_get";

typedef struct {
    char *buf;
    int   len;
    int   max_len;
} http_body_t;

static esp_err_t http_get_event_handler(esp_http_client_event_t *evt)
{
    http_body_t *body = (http_body_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && body->len < body->max_len - 1) {
        int to_copy = evt->data_len;
        if (body->len + to_copy >= body->max_len) {
            to_copy = body->max_len - 1 - body->len;
        }
        memcpy(body->buf + body->len, evt->data, to_copy);
        body->len += to_copy;
        body->buf[body->len] = '\0';
    }

    return ESP_OK;
}

esp_err_t tool_http_get_execute(const char *input_json, char *output, size_t output_size)
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

    char url[512];
    strlcpy(url, url_item->valuestring, sizeof(url));
    cJSON_Delete(root);

    http_body_t body = {
        .buf     = output,
        .len     = 0,
        .max_len = (int)output_size,
    };
    output[0] = '\0';

    esp_http_client_config_t config = {
        .url             = url,
        .method          = HTTP_METHOD_GET,
        .timeout_ms      = 5000,
        .event_handler   = http_get_event_handler,
        .user_data       = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,  /* no-op for plain HTTP */
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: request failed (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        return err;
    }

    if (body.len == 0) {
        snprintf(output, output_size, "HTTP %d (empty response)", status);
    }

    ESP_LOGI(TAG, "GET %s → HTTP %d (%d bytes)", url, status, body.len);
    return ESP_OK;
}
