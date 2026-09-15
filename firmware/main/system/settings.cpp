#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

static nvs_handle_t s_handle = 0;
static bool s_read_write = false;

bool settings_open(const char *ns, bool read_write) {
    settings_close();

    nvs_open_mode_t mode = read_write ? NVS_READWRITE : NVS_READONLY;
    esp_err_t err = nvs_open(ns, mode, &s_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND && read_write) {
        // nvs_open in RW mode never creates the namespace itself; a first
        // write creates the underlying entries, so retry after committing.
        err = nvs_open(ns, NVS_READWRITE, &s_handle);
    }
    if (err != ESP_OK) {
        s_handle = 0;
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(err));
        }
        return false;
    }
    s_read_write = read_write;
    return true;
}

void settings_close(void) {
    if (s_handle != 0) {
        if (s_read_write) {
            nvs_commit(s_handle);
        }
        nvs_close(s_handle);
        s_handle = 0;
        s_read_write = false;
    }
}

size_t settings_get_string(const char *key, char *out_buf, size_t buf_len,
                           const char *default_value) {
    if (s_handle == 0 || out_buf == NULL || buf_len == 0) {
        return 0;
    }

    size_t len = 0;
    esp_err_t err = nvs_get_str(s_handle, key, NULL, &len);
    if (err == ESP_OK && len <= buf_len) {
        err = nvs_get_str(s_handle, key, out_buf, &len);
        if (err == ESP_OK) {
            return len;
        }
    }
    // Missing key or buffer too small: fall back to the default.
    if (default_value != NULL) {
        strncpy(out_buf, default_value, buf_len - 1);
        out_buf[buf_len - 1] = '\0';
        return strlen(out_buf);
    }
    out_buf[0] = '\0';
    return 0;
}

bool settings_set_string(const char *key, const char *value) {
    if (s_handle == 0 || !s_read_write || value == NULL) {
        return false;
    }
    esp_err_t err = nvs_set_str(s_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_string(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

int32_t settings_get_int(const char *key, int32_t default_value) {
    if (s_handle == 0) {
        return default_value;
    }
    int32_t value = default_value;
    if (nvs_get_i32(s_handle, key, &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

bool settings_set_int(const char *key, int32_t value) {
    if (s_handle == 0 || !s_read_write) {
        return false;
    }
    esp_err_t err = nvs_set_i32(s_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_int(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool settings_get_bool(const char *key, bool default_value) {
    if (s_handle == 0) {
        return default_value;
    }
    uint8_t value = default_value ? 1 : 0;
    if (nvs_get_u8(s_handle, key, &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

bool settings_set_bool(const char *key, bool value) {
    if (s_handle == 0 || !s_read_write) {
        return false;
    }
    esp_err_t err = nvs_set_u8(s_handle, key, value ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_bool(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool settings_erase_key(const char *key) {
    if (s_handle == 0 || !s_read_write) {
        return false;
    }
    esp_err_t err = nvs_erase_key(s_handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    return err == ESP_OK;
}

bool settings_erase_all(void) {
    if (s_handle == 0 || !s_read_write) {
        return false;
    }
    esp_err_t err = nvs_erase_all(s_handle);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    return err == ESP_OK;
}
