#include "llm_proxy.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "llm";

static char s_api_key[128] = {0};
static char s_model[64] = MIMI_LLM_DEFAULT_MODEL;

/* Streaming response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    /* SSE line buffer for partial lines */
    char line_buf[1024];
    size_t line_len;
    /* Accumulated response text */
    char *response;
    size_t resp_len;
    size_t resp_cap;
} sse_ctx_t;

static void sse_process_line(sse_ctx_t *ctx, const char *line)
{
    /* SSE format: "data: {...}" */
    if (strncmp(line, "data: ", 6) != 0) return;
    const char *json_str = line + 6;

    /* Check for stream end */
    if (strcmp(json_str, "[DONE]") == 0) return;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "content_block_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (delta) {
            cJSON *delta_type = cJSON_GetObjectItem(delta, "type");
            if (delta_type && strcmp(delta_type->valuestring, "text_delta") == 0) {
                cJSON *text = cJSON_GetObjectItem(delta, "text");
                if (text && cJSON_IsString(text)) {
                    size_t tlen = strlen(text->valuestring);
                    /* Grow response buffer if needed */
                    while (ctx->resp_len + tlen >= ctx->resp_cap) {
                        size_t new_cap = ctx->resp_cap * 2;
                        char *tmp = realloc(ctx->response, new_cap);
                        if (!tmp) break;
                        ctx->response = tmp;
                        ctx->resp_cap = new_cap;
                    }
                    memcpy(ctx->response + ctx->resp_len, text->valuestring, tlen);
                    ctx->resp_len += tlen;
                    ctx->response[ctx->resp_len] = '\0';
                }
            }
        }
    } else if (strcmp(type->valuestring, "error") == 0) {
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (error) {
            cJSON *msg = cJSON_GetObjectItem(error, "message");
            if (msg && cJSON_IsString(msg)) {
                ESP_LOGE(TAG, "API error: %s", msg->valuestring);
            }
        }
    }

    cJSON_Delete(root);
}

static void sse_feed(sse_ctx_t *ctx, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            ctx->line_buf[ctx->line_len] = '\0';
            if (ctx->line_len > 0) {
                sse_process_line(ctx, ctx->line_buf);
            }
            ctx->line_len = 0;
        } else if (c != '\r') {
            if (ctx->line_len < sizeof(ctx->line_buf) - 1) {
                ctx->line_buf[ctx->line_len++] = c;
            }
        }
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    sse_ctx_t *ctx = (sse_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        sse_feed(ctx, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

esp_err_t llm_proxy_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t len = sizeof(s_api_key);
        nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, s_api_key, &len);

        len = sizeof(s_model);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_MODEL, s_model, &len) != ESP_OK) {
            strncpy(s_model, MIMI_LLM_DEFAULT_MODEL, sizeof(s_model) - 1);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "LLM proxy initialized (model: %s)", s_model);
    } else {
        ESP_LOGW(TAG, "No API key. Use CLI: set_api_key <KEY>");
    }
    return ESP_OK;
}

esp_err_t llm_chat(const char *system_prompt, const char *messages_json,
                   char *response_buf, size_t buf_size)
{
    if (s_api_key[0] == '\0') {
        snprintf(response_buf, buf_size, "Error: No API key configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build request body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    cJSON_AddBoolToObject(body, "stream", 1);

    /* System prompt — Anthropic format: top-level "system" field */
    cJSON_AddStringToObject(body, "system", system_prompt);

    /* Messages array (parse from JSON string) */
    cJSON *messages = cJSON_Parse(messages_json);
    if (messages) {
        cJSON_AddItemToObject(body, "messages", messages);
    } else {
        /* Fallback: single user message */
        cJSON *arr = cJSON_CreateArray();
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", messages_json);
        cJSON_AddItemToArray(arr, msg);
        cJSON_AddItemToObject(body, "messages", arr);
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!post_data) {
        snprintf(response_buf, buf_size, "Error: Failed to build request");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Calling Claude API (model: %s, body: %d bytes)", s_model, (int)strlen(post_data));

    /* SSE context */
    sse_ctx_t ctx = {0};
    ctx.response = calloc(1, MIMI_LLM_STREAM_BUF_SIZE);
    ctx.resp_cap = MIMI_LLM_STREAM_BUF_SIZE;
    if (!ctx.response) {
        free(post_data);
        snprintf(response_buf, buf_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = MIMI_LLM_API_URL,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 120 * 1000,   /* 2 min timeout for long responses */
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        free(ctx.response);
        snprintf(response_buf, buf_size, "Error: HTTP client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-api-key", s_api_key);
    esp_http_client_set_header(client, "anthropic-version", MIMI_LLM_API_VERSION);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(ctx.response);
        snprintf(response_buf, buf_size, "Error: HTTP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "API returned status %d", status);
        if (ctx.resp_len > 0) {
            /* Response might contain error info */
            snprintf(response_buf, buf_size, "API error (HTTP %d): %.200s", status, ctx.response);
        } else {
            snprintf(response_buf, buf_size, "API error (HTTP %d)", status);
        }
        free(ctx.response);
        return ESP_FAIL;
    }

    /* Copy accumulated response */
    if (ctx.resp_len > 0) {
        strncpy(response_buf, ctx.response, buf_size - 1);
        response_buf[buf_size - 1] = '\0';
        ESP_LOGI(TAG, "Claude response: %d bytes", (int)ctx.resp_len);
    } else {
        snprintf(response_buf, buf_size, "No response from Claude API");
    }

    free(ctx.response);
    return ESP_OK;
}

esp_err_t llm_set_api_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    ESP_LOGI(TAG, "API key saved");
    return ESP_OK;
}

esp_err_t llm_set_model(const char *model)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_MODEL, model));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_model, model, sizeof(s_model) - 1);
    ESP_LOGI(TAG, "Model set to: %s", s_model);
    return ESP_OK;
}
