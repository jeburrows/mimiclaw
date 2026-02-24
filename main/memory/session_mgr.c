#include "session_mgr.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "session";

static void session_path(const char *chat_id, char *buf, size_t size)
{
    snprintf(buf, size, "%s/tg_%s.jsonl", MIMI_SPIFFS_SESSION_DIR, chat_id);
}

esp_err_t session_mgr_init(void)
{
    ESP_LOGI(TAG, "Session manager initialized at %s", MIMI_SPIFFS_SESSION_DIR);
    return ESP_OK;
}

esp_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s", path);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No history yet */
        snprintf(buf, size, "[]");
        return ESP_OK;
    }

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON *messages[MIMI_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Ring buffer: overwrite oldest if full */
        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);

    /* Build JSON array with only role + content */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role    = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (role && cJSON_IsString(role) && content) {
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            if (cJSON_IsString(content)) {
                /* Try to parse as a JSON array (tool_use / tool_result records
                 * are stored as serialised JSON strings). Fall back to plain
                 * string if parsing fails or the result is not an array. */
                cJSON *parsed = cJSON_Parse(content->valuestring);
                if (parsed && cJSON_IsArray(parsed)) {
                    cJSON_AddItemToObject(entry, "content", parsed);
                } else {
                    cJSON_Delete(parsed);
                    cJSON_AddStringToObject(entry, "content", content->valuestring);
                }
            } else {
                /* Already a structured value (shouldn't happen often, but handle gracefully) */
                cJSON_AddItemToObject(entry, "content", cJSON_Duplicate(content, 1));
            }
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }

    /* Strip orphaned tool_use/tool_result blocks that occur when the ring
     * buffer slices a paired sequence.  Both cases produce API errors:
     *
     *  (a) Leading user message whose content is a tool_result array —
     *      the preceding assistant tool_use was evicted by the ring buffer.
     *
     *  (b) Trailing assistant message whose content contains only tool_use
     *      blocks with no following user tool_result message.
     */

    /* (a) Remove any leading orphaned tool_result user messages */
    while (cJSON_GetArraySize(arr) > 0) {
        cJSON *first   = cJSON_GetArrayItem(arr, 0);
        cJSON *f_role  = cJSON_GetObjectItem(first, "role");
        cJSON *f_cont  = cJSON_GetObjectItem(first, "content");
        if (f_role && cJSON_IsString(f_role) &&
            strcmp(f_role->valuestring, "user") == 0 &&
            f_cont && cJSON_IsArray(f_cont)) {
            cJSON *blk0 = cJSON_GetArrayItem(f_cont, 0);
            cJSON *t    = blk0 ? cJSON_GetObjectItem(blk0, "type") : NULL;
            if (t && cJSON_IsString(t) &&
                strcmp(t->valuestring, "tool_result") == 0) {
                ESP_LOGW(TAG, "Dropping orphaned leading tool_result block");
                cJSON_DeleteItemFromArray(arr, 0);
                continue;
            }
        }
        break;
    }

    /* (b) Remove trailing assistant message that only contains tool_use */
    int arr_size = cJSON_GetArraySize(arr);
    if (arr_size > 0) {
        cJSON *last   = cJSON_GetArrayItem(arr, arr_size - 1);
        cJSON *l_role = cJSON_GetObjectItem(last, "role");
        cJSON *l_cont = cJSON_GetObjectItem(last, "content");
        if (l_role && cJSON_IsString(l_role) &&
            strcmp(l_role->valuestring, "assistant") == 0 &&
            l_cont && cJSON_IsArray(l_cont)) {
            bool has_tool_use = false;
            bool has_text     = false;
            cJSON *blk;
            cJSON_ArrayForEach(blk, l_cont) {
                cJSON *t = cJSON_GetObjectItem(blk, "type");
                if (t && cJSON_IsString(t)) {
                    if (strcmp(t->valuestring, "tool_use") == 0) has_tool_use = true;
                    if (strcmp(t->valuestring, "text")     == 0) has_text     = true;
                }
            }
            if (has_tool_use && !has_text) {
                ESP_LOGW(TAG, "Dropping orphaned trailing tool_use block");
                cJSON_DeleteItemFromArray(arr, arr_size - 1);
            }
        }
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return ESP_OK;
}

esp_err_t session_clear(const char *chat_id)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    if (remove(path) == 0) {
        ESP_LOGI(TAG, "Session %s cleared", chat_id);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void session_list(void)
{
    DIR *dir = opendir(MIMI_SPIFFS_SESSION_DIR);
    if (!dir) {
        /* SPIFFS is flat, so list all files matching pattern */
        dir = opendir(MIMI_SPIFFS_BASE);
        if (!dir) {
            ESP_LOGW(TAG, "Cannot open SPIFFS directory");
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "tg_") && strstr(entry->d_name, ".jsonl")) {
            ESP_LOGI(TAG, "  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}
