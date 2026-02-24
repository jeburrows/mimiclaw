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
 * Returns HTTP status code on success, -1 on transport error.
 * Non-2xx: writes "HTTP <N> error: <body>" into out and returns the status.
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
        char raw[256];
        strlcpy(raw, out, sizeof(raw));
        snprintf(out, out_size, "HTTP %d error: %.240s", status, raw);
    }

    return status;
}

/* ── URL builder ─────────────────────────────────────────────── */

static void build_url(char *buf, size_t size,
                      const char *base_url, const char *env_id,
                      const char *path)
{
    snprintf(buf, size, "%s/api/environments/%s%s", base_url, env_id, path);
}

/* ── Response helpers ────────────────────────────────────────── */

/*
 * All Arcane list responses: { success, data: [...], counts: {…}, pagination: {…} }
 * Parse root, extract data array + optional total item count from pagination.
 * Returns detached data array (caller must cJSON_Delete), or NULL on error.
 * If total_out is non-NULL, fills it with grandTotalItems (or 0 if absent).
 */
static cJSON *parse_list_response(const char *resp, int *total_out,
                                   char *errbuf, size_t errbuf_size)
{
    if (total_out) *total_out = 0;

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        snprintf(errbuf, errbuf_size, "Error: JSON parse failed — %.120s", resp);
        return NULL;
    }

    cJSON *success = cJSON_GetObjectItem(root, "success");
    if (success && cJSON_IsFalse(success)) {
        snprintf(errbuf, errbuf_size, "Error: API returned success=false — %.200s", resp);
        cJSON_Delete(root);
        return NULL;
    }

    if (total_out) {
        cJSON *pg = cJSON_GetObjectItem(root, "pagination");
        if (pg) {
            cJSON *gti = cJSON_GetObjectItem(pg, "grandTotalItems");
            if (gti && cJSON_IsNumber(gti)) *total_out = (int)gti->valuedouble;
        }
    }

    cJSON *data = cJSON_DetachItemFromObject(root, "data");
    cJSON_Delete(root);

    if (!data) {
        snprintf(errbuf, errbuf_size, "Error: no 'data' field in response — %.120s", resp);
        return NULL;
    }
    if (!cJSON_IsArray(data)) {
        snprintf(errbuf, errbuf_size, "Error: 'data' is not an array");
        cJSON_Delete(data);
        return NULL;
    }
    return data;
}

/*
 * Counts-only response: { success, data: { runningContainers, … } }
 * Returns detached data object (caller must cJSON_Delete), or NULL on error.
 */
static cJSON *parse_counts_response(const char *resp, char *errbuf, size_t errbuf_size)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        snprintf(errbuf, errbuf_size, "Error: JSON parse failed — %.120s", resp);
        return NULL;
    }
    cJSON *data = cJSON_DetachItemFromObject(root, "data");
    cJSON_Delete(root);
    if (!data) {
        snprintf(errbuf, errbuf_size, "Error: no 'data' in response — %.120s", resp);
    }
    return data;
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

static void action_counts(const char *base_url, const char *env_id, const char *api_key,
                          char *output, size_t output_size)
{
    char url[256];
    char resp[512];

    build_url(url, sizeof(url), base_url, env_id, "/containers/counts");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) { strlcpy(output, resp, output_size); return; }

    cJSON *data = parse_counts_response(resp, output, output_size);
    if (!data) return;

    cJSON *r = cJSON_GetObjectItem(data, "runningContainers");
    cJSON *s = cJSON_GetObjectItem(data, "stoppedContainers");
    cJSON *t = cJSON_GetObjectItem(data, "totalContainers");
    snprintf(output, output_size, "Running: %d, Stopped: %d, Total: %d",
             (r && cJSON_IsNumber(r)) ? (int)r->valuedouble : -1,
             (s && cJSON_IsNumber(s)) ? (int)s->valuedouble : -1,
             (t && cJSON_IsNumber(t)) ? (int)t->valuedouble : -1);
    cJSON_Delete(data);
}

static void action_status(const char *base_url, const char *env_id, const char *api_key,
                          char *output, size_t output_size)
{
    char url[256];
    char resp[512];

    build_url(url, sizeof(url), base_url, env_id, "/containers/counts");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) { strlcpy(output, resp, output_size); return; }

    cJSON *data = parse_counts_response(resp, output, output_size);
    if (!data) return;

    int running_c = 0, stopped_c = 0, total_c = 0;
    cJSON *r = cJSON_GetObjectItem(data, "runningContainers");
    cJSON *s = cJSON_GetObjectItem(data, "stoppedContainers");
    cJSON *t = cJSON_GetObjectItem(data, "totalContainers");
    if (r && cJSON_IsNumber(r)) running_c = (int)r->valuedouble;
    if (s && cJSON_IsNumber(s)) stopped_c = (int)s->valuedouble;
    if (t && cJSON_IsNumber(t)) total_c   = (int)t->valuedouble;
    cJSON_Delete(data);

    build_url(url, sizeof(url), base_url, env_id, "/projects/counts");
    st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));

    int running_p = 0, total_p = 0;
    if (st >= 200 && st < 300) {
        cJSON *pd = parse_counts_response(resp, output, output_size);
        if (pd) {
            cJSON *pr = cJSON_GetObjectItem(pd, "runningProjects");
            if (!pr) pr = cJSON_GetObjectItem(pd, "running");
            cJSON *pt = cJSON_GetObjectItem(pd, "totalProjects");
            if (!pt) pt = cJSON_GetObjectItem(pd, "total");
            if (pr && cJSON_IsNumber(pr)) running_p = (int)pr->valuedouble;
            if (pt && cJSON_IsNumber(pt)) total_p   = (int)pt->valuedouble;
            cJSON_Delete(pd);
        }
    }

    snprintf(output, output_size,
             "Docker: %d running, %d stopped (%d total containers). "
             "Stacks: %d/%d running.",
             running_c, stopped_c, total_c, running_p, total_p);
}

/*
 * List containers — fetches first page (limit=20) to keep response size
 * manageable. Full container JSON (labels, networks, mounts) can be 2-3 KB
 * per container; fetching all at once is impractical on embedded hardware.
 * The response includes totals from the pagination envelope.
 */
static void action_containers(const char *base_url, const char *env_id, const char *api_key,
                              char *output, size_t output_size)
{
    char url[256];
    const size_t resp_size = 16 * 1024;
    char *resp = (char *)malloc(resp_size);
    if (!resp) { snprintf(output, output_size, "Error: out of memory"); return; }

    build_url(url, sizeof(url), base_url, env_id, "/containers?limit=20&order=asc");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, resp_size);
    if (st < 200 || st >= 300) { strlcpy(output, resp, output_size); free(resp); return; }

    int grand_total = 0;
    cJSON *data = parse_list_response(resp, &grand_total, output, output_size);
    free(resp);
    if (!data) return;

    size_t off = 0;
    int count = cJSON_GetArraySize(data);
    for (int i = 0; i < count && off < output_size - 128; i++) {
        cJSON *c = cJSON_GetArrayItem(data, i);

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
    cJSON_Delete(data);

    if (off == 0) {
        snprintf(output, output_size, "No containers found.");
        return;
    }

    /* Trim trailing newline, then append totals */
    if (output[off - 1] == '\n') off--;
    if (grand_total > count) {
        off += snprintf(output + off, output_size - off,
                        "\n(Showing %d of %d total)", count, grand_total);
    }
    output[off] = '\0';
}

static void action_stacks(const char *base_url, const char *env_id, const char *api_key,
                          char *output, size_t output_size)
{
    char url[256];
    char resp[4096];

    build_url(url, sizeof(url), base_url, env_id, "/projects?limit=100");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) { strlcpy(output, resp, output_size); return; }

    cJSON *data = parse_list_response(resp, NULL, output, output_size);
    if (!data) return;

    size_t off = 0;
    int count = cJSON_GetArraySize(data);
    for (int i = 0; i < count && off < output_size - 128; i++) {
        cJSON *p = cJSON_GetArrayItem(data, i);

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
    cJSON_Delete(data);

    if (off == 0) {
        snprintf(output, output_size, "No stacks found.");
    } else if (output[off - 1] == '\n') {
        output[off - 1] = '\0';
    }
}

/*
 * Use ?search=<name> so we only download the matching container(s) instead
 * of the full list (which is too large to parse on embedded hardware).
 */
static void action_container_lifecycle(const char *base_url, const char *env_id,
                                       const char *api_key, const char *action,
                                       const char *name, char *output, size_t output_size)
{
    char url[320];
    char path[192];
    char resp[4096];

    snprintf(path, sizeof(path), "/containers?search=%s&limit=5", name);
    build_url(url, sizeof(url), base_url, env_id, path);
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) { strlcpy(output, resp, output_size); return; }

    cJSON *data = parse_list_response(resp, NULL, output, output_size);
    if (!data) return;

    char id_buf[128] = {0};
    int count = cJSON_GetArraySize(data);
    for (int i = 0; i < count; i++) {
        cJSON *c = cJSON_GetArrayItem(data, i);
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
    cJSON_Delete(data);

    if (!id_buf[0]) {
        snprintf(output, output_size,
                 "Error: container '%s' not found (searched %d results)", name, count);
        return;
    }

    char small_resp[512];
    char lpath[256];
    snprintf(lpath, sizeof(lpath), "/containers/%s/%s", id_buf, action);
    build_url(url, sizeof(url), base_url, env_id, lpath);
    arcane_request(url, HTTP_METHOD_POST, api_key, small_resp, sizeof(small_resp));

    snprintf(output, output_size, "Container '%s' %s: %s", name, action,
             small_resp[0] ? small_resp : "OK");
}

static void action_stack_lifecycle(const char *base_url, const char *env_id,
                                   const char *api_key, const char *action,
                                   const char *name, char *output, size_t output_size)
{
    char url[256];
    char resp[4096];

    build_url(url, sizeof(url), base_url, env_id, "/projects?limit=100");
    int st = arcane_request(url, HTTP_METHOD_GET, api_key, resp, sizeof(resp));
    if (st < 200 || st >= 300) { strlcpy(output, resp, output_size); return; }

    cJSON *data = parse_list_response(resp, NULL, output, output_size);
    if (!data) return;

    char id_buf[128] = {0};
    int count = cJSON_GetArraySize(data);
    for (int i = 0; i < count; i++) {
        cJSON *p = cJSON_GetArrayItem(data, i);
        cJSON *name_item = cJSON_GetObjectItem(p, "name");
        if (!name_item || !cJSON_IsString(name_item)) continue;
        if (strcasecmp(name_item->valuestring, name) == 0) {
            get_id_string(p, id_buf, sizeof(id_buf));
            break;
        }
    }
    cJSON_Delete(data);

    if (!id_buf[0]) {
        snprintf(output, output_size, "Error: stack '%s' not found", name);
        return;
    }

    const char *verb = action;
    if (strcmp(action, "stack_start") == 0)        verb = "up";
    else if (strcmp(action, "stack_stop") == 0)    verb = "down";
    else if (strcmp(action, "stack_restart") == 0) verb = "restart";

    char small_resp[512];
    char path[256];
    snprintf(path, sizeof(path), "/projects/%s/%s", id_buf, verb);
    build_url(url, sizeof(url), base_url, env_id, path);
    arcane_request(url, HTTP_METHOD_POST, api_key, small_resp, sizeof(small_resp));

    snprintf(output, output_size, "Stack '%s' %s: %s", name, verb,
             small_resp[0] ? small_resp : "OK");
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

    if (strcmp(action, "counts") == 0) {
        action_counts(base_url, env_id, api_key, output, output_size);

    } else if (strcmp(action, "status") == 0) {
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
                 "Error: unknown action '%s'. Valid: counts, status, containers, stacks, "
                 "start, stop, restart, stack_start, stack_stop, stack_restart",
                 action);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
