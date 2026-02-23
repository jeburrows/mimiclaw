#include "tool_wled.h"
#include "tool_http_get.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_wled";

#define WLED_IP_FILE    "/spiffs/config/wled_ip.txt"
#define WLED_IP_MAX_LEN 64

static esp_err_t read_wled_ip(char *ip_buf, size_t ip_size)
{
    FILE *f = fopen(WLED_IP_FILE, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    size_t n = fread(ip_buf, 1, ip_size - 1, f);
    fclose(f);
    ip_buf[n] = '\0';

    /* Trim trailing whitespace/newlines */
    while (n > 0 && (ip_buf[n-1] == '\n' || ip_buf[n-1] == '\r' || ip_buf[n-1] == ' ')) {
        ip_buf[--n] = '\0';
    }

    return (n > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t tool_wled_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* Get action */
    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    const char *action = (action_item && cJSON_IsString(action_item))
                         ? action_item->valuestring : "";

    /* Get WLED IP: config file first, then fallback to 'wled_ip' param */
    char wled_ip[WLED_IP_MAX_LEN] = {0};
    if (read_wled_ip(wled_ip, sizeof(wled_ip)) != ESP_OK) {
        cJSON *ip_item = cJSON_GetObjectItem(root, "wled_ip");
        if (ip_item && cJSON_IsString(ip_item) && ip_item->valuestring[0]) {
            strlcpy(wled_ip, ip_item->valuestring, sizeof(wled_ip));
        }
    }

    if (wled_ip[0] == '\0') {
        cJSON_Delete(root);
        snprintf(output, output_size,
            "Error: WLED IP not configured. "
            "Save it once with: write_file path=\"/spiffs/config/wled_ip.txt\" content=\"192.168.x.x\"");
        return ESP_ERR_NOT_FOUND;
    }

    /* Build base URL */
    char url[256];
    int url_len = snprintf(url, sizeof(url), "http://%s/win", wled_ip);

    /* Optional shared params */
    cJSON *brightness_item = cJSON_GetObjectItem(root, "brightness");
    int brightness = (brightness_item && cJSON_IsNumber(brightness_item))
                     ? (int)brightness_item->valuedouble : -1;

    if (strcmp(action, "on") == 0) {
        url_len += snprintf(url + url_len, sizeof(url) - url_len, "&T=1");
        if (brightness >= 0 && brightness <= 255) {
            url_len += snprintf(url + url_len, sizeof(url) - url_len, "&A=%d", brightness);
        }

    } else if (strcmp(action, "off") == 0) {
        url_len += snprintf(url + url_len, sizeof(url) - url_len, "&T=0");

    } else if (strcmp(action, "toggle") == 0) {
        url_len += snprintf(url + url_len, sizeof(url) - url_len, "&T=2");

    } else if (strcmp(action, "color") == 0) {
        cJSON *r_item = cJSON_GetObjectItem(root, "r");
        cJSON *g_item = cJSON_GetObjectItem(root, "g");
        cJSON *b_item = cJSON_GetObjectItem(root, "b");
        int r = (r_item && cJSON_IsNumber(r_item)) ? (int)r_item->valuedouble : 255;
        int g = (g_item && cJSON_IsNumber(g_item)) ? (int)g_item->valuedouble : 255;
        int b = (b_item && cJSON_IsNumber(b_item)) ? (int)b_item->valuedouble : 255;
        /* Always turn on and set FX=0 (solid) when changing color */
        url_len += snprintf(url + url_len, sizeof(url) - url_len,
                            "&T=1&FX=0&R=%d&G=%d&B=%d", r, g, b);
        if (brightness >= 0 && brightness <= 255) {
            url_len += snprintf(url + url_len, sizeof(url) - url_len, "&A=%d", brightness);
        }

    } else if (strcmp(action, "effect") == 0) {
        cJSON *fx_item = cJSON_GetObjectItem(root, "effect_id");
        cJSON *sx_item = cJSON_GetObjectItem(root, "speed");
        cJSON *ix_item = cJSON_GetObjectItem(root, "intensity");
        int fx = (fx_item && cJSON_IsNumber(fx_item)) ? (int)fx_item->valuedouble : 0;
        url_len += snprintf(url + url_len, sizeof(url) - url_len, "&T=1&FX=%d", fx);
        if (sx_item && cJSON_IsNumber(sx_item)) {
            url_len += snprintf(url + url_len, sizeof(url) - url_len,
                                "&SX=%d", (int)sx_item->valuedouble);
        }
        if (ix_item && cJSON_IsNumber(ix_item)) {
            url_len += snprintf(url + url_len, sizeof(url) - url_len,
                                "&IX=%d", (int)ix_item->valuedouble);
        }
        if (brightness >= 0 && brightness <= 255) {
            url_len += snprintf(url + url_len, sizeof(url) - url_len, "&A=%d", brightness);
        }

    } else if (strcmp(action, "brightness") == 0) {
        if (brightness < 0 || brightness > 255) {
            cJSON_Delete(root);
            snprintf(output, output_size,
                     "Error: 'brightness' action requires brightness field (0-255)");
            return ESP_ERR_INVALID_ARG;
        }
        url_len += snprintf(url + url_len, sizeof(url) - url_len, "&A=%d", brightness);

    } else if (strcmp(action, "preset") == 0) {
        cJSON *preset_item = cJSON_GetObjectItem(root, "preset");
        if (preset_item && cJSON_IsNumber(preset_item)) {
            url_len += snprintf(url + url_len, sizeof(url) - url_len,
                                "&PL=%d", (int)preset_item->valuedouble);
        }

    } else if (strcmp(action, "status") == 0) {
        /* GET /win with no params returns current state as XML */

    } else {
        cJSON_Delete(root);
        snprintf(output, output_size,
            "Error: unknown action '%s'. Valid: on, off, toggle, color, effect, brightness, preset, status",
            action);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);

    /* Delegate to http_get */
    char http_input[512];
    snprintf(http_input, sizeof(http_input), "{\"url\":\"%s\"}", url);

    ESP_LOGI(TAG, "WLED %s → %s", action, url);
    return tool_http_get_execute(http_input, output, output_size);
}
