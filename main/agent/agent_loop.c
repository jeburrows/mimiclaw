#include "agent_loop.h"
#include "agent/context_builder.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE  (8 * 1024)

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

/* Build a compact JSON string of tool_result blocks for session storage.
 * Truncates each content string to 512 chars so session lines stay small.
 * Returns a heap-allocated string the caller must free(). */
static char *build_compact_results_json(cJSON *tool_results)
{
    if (!tool_results) return NULL;
    cJSON *compact = cJSON_CreateArray();
    cJSON *item;
    cJSON_ArrayForEach(item, tool_results) {
        cJSON *type     = cJSON_GetObjectItem(item, "type");
        cJSON *tool_id  = cJSON_GetObjectItem(item, "tool_use_id");
        cJSON *content  = cJSON_GetObjectItem(item, "content");
        cJSON *block    = cJSON_CreateObject();
        if (type    && cJSON_IsString(type))    cJSON_AddStringToObject(block, "type",        type->valuestring);
        if (tool_id && cJSON_IsString(tool_id)) cJSON_AddStringToObject(block, "tool_use_id", tool_id->valuestring);
        if (content && cJSON_IsString(content)) {
            /* Truncate large results — session needs the summary, not the full blob */
            char truncated[513];
            strlcpy(truncated, content->valuestring, sizeof(truncated));
            cJSON_AddStringToObject(block, "content", truncated);
        }
        cJSON_AddItemToArray(compact, block);
    }
    char *result = cJSON_PrintUnformatted(compact);
    cJSON_Delete(compact);
    return result;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void append_turn_context_prompt(char *prompt, size_t size, const mimi_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) {
        return;
    }

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) {
        return;
    }

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "- If using cron_add for Telegram in this turn, set channel='telegram' and chat_id to source_chat_id.\n"
        "- Never use chat_id 'cron' for Telegram messages.\n",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const mimi_msg_t *msg)
{
    if (!call || !msg || strcmp(call->name, "cron_add") != 0) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    bool changed = false;

    cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
    const char *channel = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
        json_set_string(root, "channel", msg->channel);
        channel = msg->channel;
        changed = true;
    }

    if (channel && strcmp(channel, MIMI_CHAN_TELEGRAM) == 0 &&
        strcmp(msg->channel, MIMI_CHAN_TELEGRAM) == 0 && msg->chat_id[0] != '\0') {
        cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
        const char *chat_id = cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
        if (!chat_id || chat_id[0] == '\0' || strcmp(chat_id, "cron") == 0) {
            json_set_string(root, "chat_id", msg->chat_id);
            changed = true;
        }
    }

    char *patched = NULL;
    if (changed) {
        patched = cJSON_PrintUnformatted(root);
        if (patched) {
            ESP_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel, msg->chat_id);
        }
    }

    cJSON_Delete(root);
    return patched;
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, const mimi_msg_t *msg,
                                 char *tool_output, size_t tool_output_size)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, msg);
        if (patched_input) {
            tool_input = patched_input;
        }

        /* Execute tool */
        tool_output[0] = '\0';
        tool_registry_execute(call->name, tool_input, tool_output, tool_output_size);
        free(patched_input);

        ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name, (int)strlen(tool_output));

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, MIMI_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *history_json = heap_caps_calloc(1, MIMI_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !history_json || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);

        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);
        append_turn_context_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE, &msg);
        ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        /* 2. Load session history into cJSON array */
        session_get_history_json(msg.chat_id, history_json,
                                 MIMI_LLM_STREAM_BUF_SIZE, MIMI_AGENT_MAX_HISTORY);

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) messages = cJSON_CreateArray();

        /* 3. Append current user message */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool sent_working_status = false;

        /* Collect tool call pairs (assistant tool_use + user tool_result) so we
         * can save them to session history after the turn completes.  Storing
         * real tool_use/tool_result API messages is the only reliable way to
         * let the model see evidence of prior tool calls in future turns. */
        struct { char *asst_json; char *result_json; } tc_pairs[MIMI_AGENT_MAX_TOOL_ITER];
        int tc_count = 0;
        memset(tc_pairs, 0, sizeof(tc_pairs));

        while (iteration < MIMI_AGENT_MAX_TOOL_ITER) {
            /* Send "working" indicator before each API call */
#if MIMI_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status && strcmp(msg.channel, MIMI_CHAN_SYSTEM) != 0) {
                mimi_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup("thinking...");
                if (status.content) {
                    if (message_bus_push_outbound(&status) != ESP_OK) {
                        ESP_LOGW(TAG, "Outbound queue full, drop working status");
                        free(status.content);
                    } else {
                        sent_working_status = true;
                    }
                }
            }
#endif

            llm_response_t resp;
            err = llm_chat_tools(system_prompt, messages, tools_json, &resp);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                }
                llm_response_free(&resp);
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);

            /* Build assistant tool_use content and serialise for session
             * BEFORE transferring ownership to the messages array. */
            cJSON *asst_content = build_assistant_content(&resp);
            char  *asst_for_session = cJSON_PrintUnformatted(asst_content);

            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", asst_content); /* ownership transferred */
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and serialise results for session
             * BEFORE transferring ownership to the messages array. */
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE);
            char  *results_for_session = build_compact_results_json(tool_results);

            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results); /* ownership transferred */
            cJSON_AddItemToArray(messages, result_msg);

            /* Store serialised pair for atomic session save after turn ends */
            if (tc_count < MIMI_AGENT_MAX_TOOL_ITER) {
                tc_pairs[tc_count].asst_json   = asst_for_session;
                tc_pairs[tc_count].result_json = results_for_session;
                tc_count++;
            } else {
                free(asst_for_session);
                free(results_for_session);
            }

            llm_response_free(&resp);
            iteration++;
        }

        cJSON_Delete(messages);

        /* 5. Send response */
        if (final_text && final_text[0]) {
            /* Save the complete turn to session atomically:
             *   user message → [tool_use + tool_result pairs] → final assistant text
             *
             * tool_use and tool_result records are stored as serialised JSON
             * array strings.  session_get_history_json detects these and
             * reconstructs them as proper content arrays, giving the model
             * real structured evidence of prior tool calls in future turns.
             * This prevents the model from pattern-matching text responses
             * as a substitute for actually calling tools. */
            session_append(msg.chat_id, "user", msg.content);
            for (int i = 0; i < tc_count; i++) {
                if (tc_pairs[i].asst_json)
                    session_append(msg.chat_id, "assistant", tc_pairs[i].asst_json);
                if (tc_pairs[i].result_json)
                    session_append(msg.chat_id, "user", tc_pairs[i].result_json);
            }
            esp_err_t save_asst = session_append(msg.chat_id, "assistant", final_text);
            if (save_asst != ESP_OK) {
                ESP_LOGW(TAG, "Session save failed for chat %s", msg.chat_id);
            } else {
                ESP_LOGI(TAG, "Session saved for chat %s (%d tool pairs)", msg.chat_id, tc_count);
            }

            /* Push response to outbound */
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = final_text;  /* transfer ownership */
            ESP_LOGI(TAG, "Queue final response to %s:%s (%d bytes)",
                     out.channel, out.chat_id, (int)strlen(final_text));
            if (message_bus_push_outbound(&out) != ESP_OK) {
                ESP_LOGW(TAG, "Outbound queue full, drop final response");
                free(final_text);
            } else {
                final_text = NULL;
            }
        } else {
            /* Error or empty response */
            free(final_text);
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup("Sorry, I encountered an error.");
            if (out.content) {
                if (message_bus_push_outbound(&out) != ESP_OK) {
                    ESP_LOGW(TAG, "Outbound queue full, drop error response");
                    free(out.content);
                }
            }
        }

        /* Free tool call pair strings (heap-allocated during react loop) */
        for (int i = 0; i < tc_count; i++) {
            free(tc_pairs[i].asst_json);
            free(tc_pairs[i].result_json);
        }

        /* Free inbound message content */
        free(msg.content);

        /* Log memory status */
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

esp_err_t agent_loop_init(void)
{
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    const uint32_t stack_candidates[] = {
        MIMI_AGENT_STACK,
        20 * 1024,
        16 * 1024,
        14 * 1024,
        12 * 1024,
    };

    for (size_t i = 0; i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
        uint32_t stack_size = stack_candidates[i];
        BaseType_t ret = xTaskCreatePinnedToCore(
            agent_loop_task, "agent_loop",
            stack_size, NULL,
            MIMI_AGENT_PRIO, NULL, MIMI_AGENT_CORE);

        if (ret == pdPASS) {
            ESP_LOGI(TAG, "agent_loop task created with stack=%u bytes", (unsigned)stack_size);
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "agent_loop create failed (stack=%u, free_internal=%u, largest_internal=%u), retrying...",
                 (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    return ESP_FAIL;
}
