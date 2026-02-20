#pragma once
#include "esp_err.h"

esp_err_t tool_ota_execute(const char *input_json, char *output, size_t output_size);
