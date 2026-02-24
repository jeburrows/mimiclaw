#include "tool_registry.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_ota.h"
#include "tools/tool_http_get.h"
#include "tools/tool_version.h"
#include "tools/tool_wled.h"
#include "tools/tool_arcane.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 15

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information. Use this when you need up-to-date facts, news, weather, or anything beyond your training data.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with /spiffs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with /spiffs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. /spiffs/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register ota_update */
    mimi_tool_t ota = {
        .name = "ota_update",
        .description = "Trigger an OTA firmware update. Call with no arguments to use the default release URL. Optionally pass a custom url. The device reboots automatically on success.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Optional custom firmware URL. Omit to use the default release.\"}}}",
        .execute = tool_ota_execute,
    };
    register_tool(&ota);

    /* Register http_get */
    mimi_tool_t hg = {
        .name = "http_get",
        .description = "Make an HTTP GET request to a URL and return the response body. Use this to call local network APIs such as WLED, Home Assistant, or any REST endpoint accessible from this device's network.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Full URL to request (http:// or https://)\"}},"
            "\"required\":[\"url\"]}",
        .execute = tool_http_get_execute,
    };
    register_tool(&hg);

    /* Register get_version */
    mimi_tool_t ver = {
        .name = "get_version",
        .description = "Get the firmware version, build date, and ESP-IDF version currently running on this device. "
                       "Always call this tool for version questions — never rely on conversation history.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_version_execute,
    };
    register_tool(&ver);

    /* Register wled_control */
    mimi_tool_t wled = {
        .name = "wled_control",
        .description = "Control WLED smart LED lights. Use for any request about lights, LEDs, "
                       "colors, brightness, or lighting effects. Handles on/off/color/effect/brightness/preset. "
                       "Requires WLED IP saved to /spiffs/config/wled_ip.txt.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"action\":{\"type\":\"string\","
              "\"description\":\"What to do: on, off, toggle, color, effect, brightness, preset, status\"},"
            "\"r\":{\"type\":\"integer\",\"description\":\"Red 0-255 (for action=color)\"},"
            "\"g\":{\"type\":\"integer\",\"description\":\"Green 0-255 (for action=color)\"},"
            "\"b\":{\"type\":\"integer\",\"description\":\"Blue 0-255 (for action=color)\"},"
            "\"brightness\":{\"type\":\"integer\",\"description\":\"Brightness 0-255\"},"
            "\"effect_id\":{\"type\":\"integer\","
              "\"description\":\"WLED effect index 0-101 (for action=effect). Common: 0=Solid, 1=Blink, 2=Breathe, 9=Rainbow, 11=Fireworks\"},"
            "\"speed\":{\"type\":\"integer\",\"description\":\"Effect speed 0-255\"},"
            "\"intensity\":{\"type\":\"integer\",\"description\":\"Effect intensity 0-255\"},"
            "\"preset\":{\"type\":\"integer\",\"description\":\"WLED preset number to load\"},"
            "\"wled_ip\":{\"type\":\"string\","
              "\"description\":\"WLED IP address (only needed if not saved in /spiffs/config/wled_ip.txt)\"}"
            "},"
            "\"required\":[\"action\"]}",
        .execute = tool_wled_execute,
    };
    register_tool(&wled);

    /* Register docker_status */
    mimi_tool_t arcane = {
        .name = "docker_status",
        .description = "Check and control Docker containers and stacks via the Arcane API. "
                       "Use for any request about Docker servers, containers, stacks, or services.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"action\":{\"type\":\"string\","
              "\"description\":\"counts (container counts only), "
              "status (counts + stack summary), containers (list first 20), stacks (list all), "
              "start/stop/restart/redeploy (container by name), "
              "vuln_scan (severity summary; triggers scan automatically if not yet scanned), "
              "vuln_list (top 5 CRITICAL/HIGH CVEs with NIST links for container), "
              "stack_start/stack_stop/stack_restart/stack_redeploy (stack by name)\"},"
            "\"name\":{\"type\":\"string\","
              "\"description\":\"Container or stack name (required for start/stop/restart/redeploy/stack_* actions)\"}"
            "},"
            "\"required\":[\"action\"]}",
        .execute = tool_arcane_execute,
    };
    register_tool(&arcane);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

/*
 * Strip bytes that would produce invalid UTF-8 when the tool result is
 * embedded in a JSON string sent to the Anthropic API.
 *
 * Specifically: cJSON can decode lone UTF-16 surrogate escape sequences
 * (\uD800–\uDFFF) from upstream JSON into WTF-8 byte sequences (0xED 0xA?/0xB?
 * 0x8?), which are not valid UTF-8.  Any byte above 0x7E or below 0x20
 * (except \n and \t) is replaced with '?' — all meaningful tool output
 * (counts, IDs, severity levels, container names) is ASCII anyway.
 */
static void sanitize_tool_output(char *s)
{
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p > 0x7E || (*p < 0x20 && *p != '\n' && *p != '\t')) {
            *p = '?';
        }
    }
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            esp_err_t err = s_tools[i].execute(input_json, output, output_size);
            sanitize_tool_output(output);
            return err;
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
