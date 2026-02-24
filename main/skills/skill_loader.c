#include "skills/skill_loader.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"

static const char *TAG = "skills";

/* ── Built-in skill contents ─────────────────────────────────── */

#define BUILTIN_WEATHER \
    "# Weather\n" \
    "\n" \
    "Get current weather and forecasts using web_search.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks about weather, temperature, or forecasts.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time to know the current date\n" \
    "2. Use web_search with a query like \"weather in [city] today\"\n" \
    "3. Extract temperature, conditions, and forecast from results\n" \
    "4. Present in a concise, friendly format\n" \
    "\n" \
    "## Example\n" \
    "User: \"What's the weather in Tokyo?\"\n" \
    "→ get_current_time\n" \
    "→ web_search \"weather Tokyo today February 2026\"\n" \
    "→ \"Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north.\"\n"

#define BUILTIN_DAILY_BRIEFING \
    "# Daily Briefing\n" \
    "\n" \
    "Compile a personalized daily briefing for the user.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks for a daily briefing, morning update, or \"what's new today\".\n" \
    "Also useful as a heartbeat/cron task.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time for today's date\n" \
    "2. Read /spiffs/memory/MEMORY.md for user preferences and context\n" \
    "3. Read today's daily note if it exists\n" \
    "4. Use web_search for relevant news based on user interests\n" \
    "5. Compile a concise briefing covering:\n" \
    "   - Date and time\n" \
    "   - Weather (if location known from USER.md)\n" \
    "   - Relevant news/updates based on user interests\n" \
    "   - Any pending tasks from memory\n" \
    "   - Any scheduled cron jobs\n" \
    "\n" \
    "## Format\n" \
    "Keep it brief — 5-10 bullet points max. Use the user's preferred language.\n"

#define BUILTIN_SKILL_CREATOR \
    "# Skill Creator\n" \
    "\n" \
    "Create new skills for MimiClaw.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to create a new skill, teach the bot something, or add a new capability.\n" \
    "\n" \
    "## How to create a skill\n" \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n" \
    "2. Write a SKILL.md file with this structure:\n" \
    "   - `# Title` — clear name\n" \
    "   - Brief description paragraph\n" \
    "   - `## When to use` — trigger conditions\n" \
    "   - `## How to use` — step-by-step instructions\n" \
    "   - `## Example` — concrete example (optional but helpful)\n" \
    "3. Save to `/spiffs/skills/<name>.md` using write_file\n" \
    "4. The skill will be automatically available after the next conversation\n" \
    "\n" \
    "## Best practices\n" \
    "- Keep skills concise — the context window is limited\n" \
    "- Focus on WHAT to do, not HOW (the agent is smart)\n" \
    "- Include specific tool calls the agent should use\n" \
    "- Test by asking the agent to use the new skill\n" \
    "\n" \
    "## Example\n" \
    "To create a \"translate\" skill:\n" \
    "write_file path=\"/spiffs/skills/translate.md\" content=\"# Translate\\n\\nTranslate text between languages.\\n\\n" \
    "## When to use\\nWhen the user asks to translate text.\\n\\n" \
    "## How to use\\n1. Identify source and target languages\\n" \
    "2. Translate directly using your language knowledge\\n" \
    "3. For specialized terms, use web_search to verify\\n\"\n"

#define BUILTIN_OTA_UPDATE \
    "# OTA Update\n" \
    "\n" \
    "Update the device firmware over the air.\n" \
    "\n" \
    "## When to use\n" \
    "When the user says: update, upgrade, OTA, flash new firmware, latest version, or similar.\n" \
    "\n" \
    "## IMPORTANT\n" \
    "Call ota_update with NO arguments. The firmware URL is configured in the build.\n" \
    "Do NOT ask the user for a URL. Do NOT ask for confirmation. Just run the tool immediately.\n" \
    "\n" \
    "## How to use\n" \
    "1. Tell the user: \"Starting OTA download — this takes 60-120 seconds. I'll confirm when done.\"\n" \
    "2. Call ota_update({}) with no arguments\n" \
    "3. The tool returns the actual result: success (rebooting in ~5 sec) or failure (stays on current firmware)\n" \
    "4. Relay the result to the user. On success: \"OTA complete! Rebooting now — I'll be back in ~30 seconds.\"\n" \
    "\n" \
    "## After OTA\n" \
    "When the user asks what version is running after an OTA, always call get_version — never guess from history.\n" \
    "\n" \
    "## Example\n" \
    "User: \"Update the firmware\"\n" \
    "→ \"Starting OTA download — this takes 60-120 seconds. I'll confirm when done.\"\n" \
    "→ ota_update({})\n" \
    "→ [tool returns: \"OTA successful... device will reboot in ~5 seconds\"]\n" \
    "→ \"OTA complete! Rebooting now — I'll be back online in ~30 seconds.\"\n"

#define BUILTIN_WLED \
    "# WLED Control\n" \
    "\n" \
    "Control smart LED lights using the wled_control tool.\n" \
    "Use for ANY request about lights, LEDs, brightness, colors, or lighting effects.\n" \
    "Trigger words: lights, LEDs, lamp, bulb, strip, bright, dim, color, glow, on, off.\n" \
    "\n" \
    "## CRITICAL: Always call wled_control — never just describe the action\n" \
    "You MUST call wled_control for EVERY light request. No exceptions.\n" \
    "\n" \
    "## First-time setup\n" \
    "If the user has not set a WLED IP yet, ask for it once then save it:\n" \
    "write_file path=\"/spiffs/config/wled_ip.txt\" content=\"192.168.x.x\"\n" \
    "After that, wled_control finds the IP automatically — no need to ask again.\n" \
    "\n" \
    "## Actions\n" \
    "- on / off / toggle — power\n" \
    "- color — solid color (provide r, g, b 0-255). Always turns on and clears any effect.\n" \
    "- effect — lighting effect (provide effect_id 0-101)\n" \
    "  Common effects: 0=Solid, 1=Blink, 2=Breathe, 9=Rainbow, 11=Fireworks, 65=Ripple\n" \
    "- brightness — set level (provide brightness 0-255; 128=50%, 255=max)\n" \
    "- preset — load saved preset (provide preset number)\n" \
    "- status — read current state\n" \
    "\n" \
    "## Common colors (r/g/b values)\n" \
    "Red=255/0/0  Green=0/255/0  Blue=0/0/255  White=255/255/255\n" \
    "Warm=255/147/41  Purple=128/0/128  Orange=255/165/0  Pink=255/20/147\n" \
    "\n" \
    "## Examples\n" \
    "User: \"Turn on the lights\"\n" \
    "→ wled_control({\"action\": \"on\"})\n" \
    "\n" \
    "User: \"Set lights to green\"\n" \
    "→ wled_control({\"action\": \"color\", \"r\": 0, \"g\": 255, \"b\": 0})\n" \
    "\n" \
    "User: \"Blue at half brightness\"\n" \
    "→ wled_control({\"action\": \"color\", \"r\": 0, \"g\": 0, \"b\": 255, \"brightness\": 128})\n" \
    "\n" \
    "User: \"Rainbow effect\"\n" \
    "→ wled_control({\"action\": \"effect\", \"effect_id\": 9})\n" \
    "\n" \
    "User: \"Dim to 30%\"\n" \
    "→ wled_control({\"action\": \"brightness\", \"brightness\": 77})\n" \
    "\n" \
    "User: \"Turn off\"\n" \
    "→ wled_control({\"action\": \"off\"})\n"

#define BUILTIN_DOCKER \
    "# Docker Status\n" \
    "\n" \
    "Check and control Docker containers and stacks via the Arcane API.\n" \
    "Use for ANY request about Docker, containers, stacks, or services.\n" \
    "Trigger words: docker, container, stack, service, running, stopped.\n" \
    "\n" \
    "## CRITICAL: Always call docker_status — never guess or answer from memory\n" \
    "You MUST call docker_status for EVERY Docker request. No exceptions.\n" \
    "\n" \
    "## Actions\n" \
    "- counts — container counts only (Running/Stopped/Total). Fastest check.\n" \
    "- status — container counts + stack summary\n" \
    "- containers — full list of all containers with state and image\n" \
    "- stacks — full list of all stacks with service counts\n" \
    "- start / stop / restart — control a container by name\n" \
    "- redeploy — pull latest image and recreate a container by name\n" \
    "- stack_start / stack_stop / stack_restart — control a stack by name\n" \
    "- stack_redeploy — pull all images and redeploy a stack by name (takes 1-2 min)\n" \
    "\n" \
    "## Examples\n" \
    "User: \"How many containers are running?\"\n" \
    "→ docker_status({\"action\": \"counts\"})\n" \
    "\n" \
    "User: \"How's my Docker server?\"\n" \
    "→ docker_status({\"action\": \"status\"})\n" \
    "\n" \
    "User: \"List all containers\"\n" \
    "→ docker_status({\"action\": \"containers\"})\n" \
    "\n" \
    "User: \"Show my stacks\"\n" \
    "→ docker_status({\"action\": \"stacks\"})\n" \
    "\n" \
    "User: \"Restart the nginx container\"\n" \
    "→ docker_status({\"action\": \"restart\", \"name\": \"nginx\"})\n" \
    "\n" \
    "User: \"Update the nginx container\"\n" \
    "→ docker_status({\"action\": \"redeploy\", \"name\": \"nginx\"})\n" \
    "\n" \
    "User: \"Start the monitoring stack\"\n" \
    "→ docker_status({\"action\": \"stack_start\", \"name\": \"monitoring\"})\n" \
    "\n" \
    "User: \"Update the monitoring stack\"\n" \
    "→ docker_status({\"action\": \"stack_redeploy\", \"name\": \"monitoring\"})\n" \
    "\n" \
    "## Setup\n" \
    "Requires MIMI_SECRET_ARCANE_URL, MIMI_SECRET_ARCANE_API_KEY, and\n" \
    "MIMI_SECRET_ARCANE_ENV_ID set in mimi_secrets.h before building.\n"

/* Built-in skill registry */
typedef struct {
    const char *filename;   /* e.g. "weather" */
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    { "weather",        BUILTIN_WEATHER        },
    { "daily-briefing", BUILTIN_DAILY_BRIEFING },
    { "skill-creator",  BUILTIN_SKILL_CREATOR  },
    { "ota-update",     BUILTIN_OTA_UPDATE     },
    { "wled",           BUILTIN_WLED           },
    { "docker",         BUILTIN_DOCKER         },
};

#define NUM_BUILTINS (sizeof(s_builtins) / sizeof(s_builtins[0]))

/* ── Install built-in skills if missing ──────────────────────── */

static void install_builtin(const builtin_skill_t *skill)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%s.md", MIMI_SKILLS_PREFIX, skill->filename);

    /* Always overwrite builtins so OTA firmware updates propagate skill changes */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write skill: %s", path);
        return;
    }

    fputs(skill->content, f);
    fclose(f);
    ESP_LOGI(TAG, "Installed built-in skill: %s", path);
}

esp_err_t skill_loader_init(void)
{
    ESP_LOGI(TAG, "Initializing skills system");

    for (size_t i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    ESP_LOGI(TAG, "Skills system ready (%d built-in)", (int)NUM_BUILTINS);
    return ESP_OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title"
 * Returns pointer past "# " or the line itself if no prefix.
 */
static const char *extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS for skill enumeration");
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;
    /* SPIFFS readdir returns filenames relative to the mount point (e.g. "skills/weather.md").
       We match entries that start with "skills/" and end with ".md". */
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;

        /* Match files under skills/ with .md extension */
        if (strncmp(name, skills_subdir, subdir_len) != 0) continue;

        size_t name_len = strlen(name);
        if (name_len < subdir_len + 4) continue;  /* at least "skills/x.md" */
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Build full path */
        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        extract_description(f, desc, sizeof(desc));
        fclose(f);

        /* Append to summary */
        off += snprintf(buf + off, size - off,
            "- **%s**: %s (read with: read_file %s)\n",
            title, desc, full_path);
    }

    closedir(dir);

    buf[off] = '\0';
    ESP_LOGI(TAG, "Skills summary: %d bytes", (int)off);
    return off;
}

/* ── Build full skill content for system prompt ───────────────── */

size_t skill_loader_build_full(char *buf, size_t size)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;

        if (strncmp(name, skills_subdir, subdir_len) != 0) continue;
        size_t name_len = strlen(name);
        if (name_len < subdir_len + 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        off += snprintf(buf + off, size - off, "---\n");
        size_t n = fread(buf + off, 1, size - off - 1, f);
        off += n;
        buf[off] = '\0';
        if (off < size - 1 && buf[off - 1] != '\n') {
            buf[off++] = '\n';
        }
        fclose(f);
    }

    closedir(dir);
    buf[off] = '\0';
    ESP_LOGI(TAG, "Skills full content: %d bytes", (int)off);
    return off;
}
