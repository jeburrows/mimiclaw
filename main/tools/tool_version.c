#include "tool_version.h"
#include "esp_app_desc.h"

esp_err_t tool_version_execute(const char *input_json, char *output, size_t output_size)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    snprintf(output, output_size,
             "Firmware: %s\nProject: %s\nBuilt: %s %s\nESP-IDF: %s",
             desc->version,
             desc->project_name,
             desc->date,
             desc->time,
             desc->idf_ver);

    return ESP_OK;
}
