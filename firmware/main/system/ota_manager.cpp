#include "ota_manager.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

static const char *TAG = "ota_mgr";

void ota_manager_init(void) {
#if VOXIE_ENABLE_OTA
    // Mark current app as valid (cancels rollback timer from bootloader)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "OTA pending verify — marking app valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    ESP_LOGI(TAG, "OTA manager initialized (running partition: %s)", running->label);
#else
    ESP_LOGI(TAG, "OTA disabled by config");
#endif
}

esp_err_t ota_start_update(const char *url) {
#if VOXIE_ENABLE_OTA
    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    state_machine_transition(DEVICE_STATE_UPGRADING);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.timeout_ms = 30000;
    http_cfg.keep_alive_enable = true;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
        return ret;
    }

    // Download in chunks
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int progress = esp_https_ota_get_image_len_read(ota_handle);
        ESP_LOGI(TAG, "OTA progress: %d bytes downloaded", progress);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
        return ret;
    }

    // Validate and finish
    ret = esp_https_ota_finish(ota_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded! Rebooting in 1 second...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
    }

    return ret;
#else
    ESP_LOGW(TAG, "OTA disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
