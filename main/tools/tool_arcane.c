#include "tool_arcane.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "tool_arcane";

/* ── HTTP helper ─────────────────────────────────────────────── */

typedef struct {
    char *buf;
    int   len;
    int   max;
} arcane_body_t;

static esp_err_t arcane_http_event(esp_http_client_event_t *evt)
{
    arcane_body_t *body = (arcane_body_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && body->len < body->max - 1) {
        int n = evt->data_len;
        if (body->len + n >= body->max) n = body->max - 1 - body->len;
        memcpy(body->buf + body->len, evt->data, n);
        body->len += n;
        body->buf[body->len] = '\0';
    }
    return ESP_OK;
}

/*
 * Returns the HTTP status code (200, 401, 404, …) on a successful transport,
 * or -1 on a transport-level error (connection refused, timeout, etc.).
 * On non-2xx the response body (truncated) is placed in `out` as an error string.
 */
static int arcane_request(const char *url, esp_http_client_method_t method,
                          const char *api_key,
                          char *out, size_t out_size)
{
    arcane_body_t body = { .buf = out, .len = 0, .max = (int)out_size };
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = method,
        .timeout_ms    = 8000,
        .event_handler = arcane_http_event,
        .user_data     = &body,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(out, out_size, "Error: failed to init HTTP client");
        return -1;
    }

    esp_http_client_set_header(client, "X-API-Key", api_key);
    if (method == HTTP_METHOD_POST) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, "{}", 2);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        snprintf(out, out_size, "Error: transport failed (%s)", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "%s %s → HTTP %d (%d bytes)",
             method == HTTP_METHOD_GET ? "GET" : "POST", url, status, body.len);

    if (status < 200 || status >= 300) {
        /* Preserve the raw response body for debugging, prepend the status */
        char raw[256];
        strlcpy(raw, out, sizeof(raw));
        snprintf(out, out_size, "HTTP %d error: %.240s", status, raw);
    }

    return status;
}

/* ── URL builder: always includes /api prefix ─────────────────── */

static void build_url(char *buf, size_t size,
                      const char *base_url, const char *env_id,
                      const char *path)
{
    snprintf(buf, size, "%s/api/environments/%s%s", base_url, env_id, path);
}

/* ── Helper: extract id string from a cJSON object ───────────── */

static bool get_id_string(cJSON *obj, char *id_buf, size_t id_size)
{
    cJSON *id_item = cJSON_GetObjectItem(obj, "id");
    if (!id_item) return false;
    if (cJSON_IsString(id_item) && id_item->valuestring[0]) {
        strlcpy(id_buf, id_item->valuestring, id_size);
        return true;
    }
    if (cJSON_IsNumber(id_item)) {
        snprintf(id_buf, id_size, "%d", (int)id_item->valuedouble);
        return true;
    }
    return false;
}

/* ── Actions ─────────────────────────────────────────────────── */

static void action_status(const char *base_url, const char *env_id, const char *api_key,
                          char *output, size_t output_size)
{
    char url[256];
    char resp[512];

    /* Container counts */
    build_url(url, sizeof(url), base_url, env_id, "/containers/counts");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) {
        strlcpy(output, resp, output_size);
        return;
    }

    int running_c = 0, stopped_c = 0, total_c = 0;
    cJSON *cc = cJSON_Parse(resp);
    if (cc) {
        cJSON *r = cJSON_GetObjectItem(cc, "runningContainers");
        cJSON *s = cJSON_GetObjectItem(cc, "stoppedContainers");
        cJSON *t = cJSON_GetObjectItem(cc, "totalContainers");
        if (r && cJSON_IsNumber(r)) running_c = (int)r->valuedouble;
        if (s && cJSON_IsNumber(s)) stopped_c = (int)s->valuedouble;
        if (t && cJSON_IsNumber(t)) total_c   = (int)t->valuedouble;
        cJSON_Delete(cc);
    } else {
        snprintf(output, output_size, "Error: unexpected container counts response: %.200s", resp);
        return;
    }

    /* Project counts */
    build_url(url, sizeof(url), base_url, env_id, "/projects/counts");
    st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));

    int running_p = 0, total_p = 0;
    if (st >= 200 && st < 300) {
        cJSON *pc = cJSON_Parse(resp);
        if (pc) {
            cJSON *r = cJSON_GetObjectItem(pc, "runningProjects");
            if (!r) r = cJSON_GetObjectItem(pc, "running");
            cJSON *t = cJSON_GetObjectItem(pc, "totalProjects");
            if (!t) t = cJSON_GetObjectItem(pc, "total");
            if (r && cJSON_IsNumber(r)) running_p = (int)r->valuedouble;
            if (t && cJSON_IsNumber(t)) total_p   = (int)t->valuedouble;
            cJSON_Delete(pc);
        }
    }

    snprintf(output, output_size,
             "Docker: %d running, %d stopped (%d total containers). "
             "Stacks: %d/%d running.",
             running_c, stopped_c, total_c, running_p, total_p);
}

static void action_containers(const char *base_url, const char *env_id, const char *api_key,
                              char *output, size_t output_size)
{
    char url[256];
    char resp[4096];

    build_url(url, sizeof(url), base_url, env_id, "/containers");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) {
        strlcpy(output, resp, output_size);
        return;
    }

    cJSON *arr = cJSON_Parse(resp);
    if (!arr || !cJSON_IsArray(arr)) {
        snprintf(output, output_size, "Error: unexpected response: %.200s", resp);
        if (arr) cJSON_Delete(arr);
        return;
    }

    size_t off = 0;
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count && off < output_size - 128; i++) {
        cJSON *c = cJSON_GetArrayItem(arr, i);

        const char *name = "?";
        cJSON *names = cJSON_GetObjectItem(c, "names");
        if (names && cJSON_IsArray(names) && cJSON_GetArraySize(names) > 0) {
            cJSON *n0 = cJSON_GetArrayItem(names, 0);
            if (cJSON_IsString(n0)) {
                name = n0->valuestring;
                if (name[0] == '/') name++;
            }
        }

        cJSON *state_item = cJSON_GetObjectItem(c, "state");
        const char *state = (state_item && cJSON_IsString(state_item))
                            ? state_item->valuestring : "?";

        cJSON *image_item = cJSON_GetObjectItem(c, "image");
        const char *image = (image_item && cJSON_IsString(image_item))
                            ? image_item->valuestring : "?";

        off += snprintf(output + off, output_size - off,
                        "[%s] %s (%s)\n", state, name, image);
    }
    cJSON_Delete(arr);

    if (off == 0) {
        snprintf(output, output_size, "No containers found.");
    } else if (output[off - 1] == '\n') {
        output[off - 1] = '\0';
    }
}

static void action_stacks(const char *base_url, const char *env_id, const char *api_key,
                          char *output, size_t output_size)
{
    char url[256];
    char resp[4096];

    build_url(url, sizeof(url), base_url, env_id, "/projects");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) {
        strlcpy(output, resp, output_size);
        return;
    }

    cJSON *arr = cJSON_Parse(resp);
    if (!arr || !cJSON_IsArray(arr)) {
        snprintf(output, output_size, "Error: unexpected response: %.200s", resp);
        if (arr) cJSON_Delete(arr);
        return;
    }

    size_t off = 0;
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count && off < output_size - 128; i++) {
        cJSON *p = cJSON_GetArrayItem(arr, i);

        cJSON *name_item   = cJSON_GetObjectItem(p, "name");
        cJSON *status_item = cJSON_GetObjectItem(p, "status");
        cJSON *svc_item    = cJSON_GetObjectItem(p, "serviceCount");
        cJSON *run_item    = cJSON_GetObjectItem(p, "runningCount");

        const char *name   = (name_item && cJSON_IsString(name_item))
                             ? name_item->valuestring : "?";
        const char *status = (status_item && cJSON_IsString(status_item))
                             ? status_item->valuestring : "?";
        int svc = (svc_item && cJSON_IsNumber(svc_item)) ? (int)svc_item->valuedouble : 0;
        int run = (run_item && cJSON_IsNumber(run_item)) ? (int)run_item->valuedouble : 0;

        off += snprintf(output + off, output_size - off,
                        "[%s] %s (%d/%d services)\n", status, name, run, svc);
    }
    cJSON_Delete(arr);

    if (off == 0) {
        snprintf(output, output_size, "No stacks found.");
    } else if (output[off - 1] == '\n') {
        output[off - 1] = '\0';
    }
}

static void action_container_lifecycle(const char *base_url, const char *env_id,
                                       const char *api_key, const char *action,
                                       const char *name, char *output, size_t output_size)
{
    char url[256];
    char resp[4096];

    build_url(url, sizeof(url), base_url, env_id, "/containers");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) {
        strlcpy(output, resp, output_size);
        return;
    }

    cJSON *arr = cJSON_Parse(resp);
    if (!arr || !cJSON_IsArray(arr)) {
        snprintf(output, output_size, "Error: cannot list containers to find '%s'", name);
        if (arr) cJSON_Delete(arr);
        return;
    }

    char id_buf[128] = {0};
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count; i++) {
        cJSON *c = cJSON_GetArrayItem(arr, i);
        cJSON *names = cJSON_GetObjectItem(c, "names");
        if (!names || !cJSON_IsArray(names)) continue;
        int ncount = cJSON_GetArraySize(names);
        for (int j = 0; j < ncount; j++) {
            cJSON *n0 = cJSON_GetArrayItem(names, j);
            if (!cJSON_IsString(n0)) continue;
            const char *cname = n0->valuestring;
            if (cname[0] == '/') cname++;
            if (strcasecmp(cname, name) == 0) {
                get_id_string(c, id_buf, sizeof(id_buf));
                break;
            }
        }
        if (id_buf[0]) break;
    }
    cJSON_Delete(arr);

    if (!id_buf[0]) {
        snprintf(output, output_size, "Error: container '%s' not found", name);
        return;
    }

    char path[128];
    snprintf(path, sizeof(path), "/containers/%s/%s", id_buf, action);
    build_url(url, sizeof(url), base_url, env_id, path);
    arcane_request(url, HTTP_METHOD_POST, api_key, resp, sizeof(resp));

    snprintf(output, output_size, "Container '%s' %s: %s", name, action,
             resp[0] ? resp : "OK");
}

static void action_stack_lifecycle(const char *base_url, const char *env_id,
                                   const char *api_key, const char *action,
                                   const char *name, char *output, size_t output_size)
{
    char url[256];
    char resp[4096];

    build_url(url, sizeof(url), base_url, env_id, "/projects");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) {
        strlcpy(output, resp, output_size);
        return;
    }

    cJSON *arr = cJSON_Parse(resp);
    if (!arr || !cJSON_IsArray(arr)) {
        snprintf(output, output_size, "Error: cannot list stacks to find '%s'", name);
        if (arr) cJSON_Delete(arr);
        return;
    }

    char id_buf[128] = {0};
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count; i++) {
        cJSON *p = cJSON_GetArrayItem(arr, i);
        cJSON *name_item = cJSON_GetObjectItem(p, "name");
        if (!name_item || !cJSON_IsString(name_item)) continue;
        if (strcasecmp(name_item->valuestring, name) == 0) {
            get_id_string(p, id_buf, sizeof(id_buf));
            break;
        }
    }
    cJSON_Delete(arr);

    if (!id_buf[0]) {
        snprintf(output, output_size, "Error: stack '%s' not found", name);
        return;
    }

    /* Map logical action to Arcane API endpoint verb */
    const char *verb = action;
    if (strcmp(action, "stack_start") == 0)        verb = "up";
    else if (strcmp(action, "stack_stop") == 0)    verb = "down";
    else if (strcmp(action, "stack_restart") == 0) verb = "restart";

    char path[128];
    snprintf(path, sizeof(path), "/projects/%s/%s", id_buf, verb);
    build_url(url, sizeof(url), base_url, env_id, path);
    arcane_request(url, HTTP_METHOD_POST, api_key, resp, sizeof(resp));

    snprintf(output, output_size, "Stack '%s' %s: %s", name, verb,
             resp[0] ? resp : "OK");
}

/* ── Entry point ─────────────────────────────────────────────── */

esp_err_t tool_arcane_execute(const char *input_json, char *output, size_t output_size)
{
    const char *base_url = MIMI_SECRET_ARCANE_URL;
    const char *api_key  = MIMI_SECRET_ARCANE_API_KEY;
    const char *env_id   = MIMI_SECRET_ARCANE_ENV_ID;

    if (!base_url || base_url[0] == '\0') {
        snprintf(output, output_size,
                 "Error: MIMI_SECRET_ARCANE_URL not configured. Set it in mimi_secrets.h and rebuild.");
        return ESP_ERR_NOT_FOUND;
    }
    if (!api_key || api_key[0] == '\0') {
        snprintf(output, output_size,
                 "Error: MIMI_SECRET_ARCANE_API_KEY not configured. Set it in mimi_secrets.h and rebuild.");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    const char *action = (action_item && cJSON_IsString(action_item))
                         ? action_item->valuestring : "";

    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    const char *name = (name_item && cJSON_IsString(name_item))
                       ? name_item->valuestring : "";

    ESP_LOGI(TAG, "action=%s name=%s env=%s", action, name, env_id);

    if (strcmp(action, "status") == 0) {
        action_status(base_url, env_id, api_key, output, output_size);

    } else if (strcmp(action, "containers") == 0) {
        action_containers(base_url, env_id, api_key, output, output_size);

    } else if (strcmp(action, "stacks") == 0) {
        action_stacks(base_url, env_id, api_key, output, output_size);

    } else if (strcmp(action, "start")   == 0 ||
               strcmp(action, "stop")    == 0 ||
               strcmp(action, "restart") == 0) {
        if (!name[0]) {
            snprintf(output, output_size,
                     "Error: 'name' is required for container %s", action);
        } else {
            action_container_lifecycle(base_url, env_id, api_key, action, name,
                                       output, output_size);
        }

    } else if (strcmp(action, "stack_start")   == 0 ||
               strcmp(action, "stack_stop")    == 0 ||
               strcmp(action, "stack_restart") == 0) {
        if (!name[0]) {
            snprintf(output, output_size,
                     "Error: 'name' is required for stack %s", action);
        } else {
            action_stack_lifecycle(base_url, env_id, api_key, action, name,
                                   output, output_size);
        }

    } else {
        snprintf(output, output_size,
                 "Error: unknown action '%s'. Valid: status, containers, stacks, "
                 "start, stop, restart, stack_start, stack_stop, stack_restart",
                 action);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
