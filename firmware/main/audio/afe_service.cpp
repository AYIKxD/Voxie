#include "afe_service.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "audio/audio_service.h"
#include "audio/stream_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include <string.h>

static const char *TAG = "afe_svc";

static esp_afe_sr_data_t *afe_data = NULL;
static const esp_afe_sr_iface_t *afe_iface = NULL;
static srmodel_list_t *models = NULL;
static TaskHandle_t s_afe_task_handle = NULL;
static volatile bool s_running = false;
static volatile bool s_wake_word_detected = false;

extern "C" void kws_engine_push_audio(const int16_t* data, size_t samples);

static void afe_task(void *arg) {
    ESP_LOGI(TAG, "AFE task started");
    int feed_samples = afe_iface->get_feed_chunksize(afe_data);
    int16_t *raw = (int16_t*)malloc(feed_samples * sizeof(int16_t));

    while (s_running) {
        size_t samples_read = audio_service_read_mic(raw, feed_samples, 100);
        if (samples_read != feed_samples) {
            continue;
        }

        afe_iface->feed(afe_data, raw);

        afe_fetch_result_t *result = afe_iface->fetch(afe_data);
        if (result != NULL && result->ret_value == ESP_OK) {
            stream_service_push_mic(result->data, result->data_size / sizeof(int16_t));
            
#if VOXIE_HAS_TFLITE
            // Provide clean audio to kws_engine as well
            kws_engine_push_audio(result->data, result->data_size / sizeof(int16_t));
#endif

            if (result->wakeup_state == WAKENET_DETECTED) {
                s_wake_word_detected = true;
                device_state_t st = state_machine_get_state();
                if (st == DEVICE_STATE_IDLE_LISTENING || st == DEVICE_STATE_PLAYING_REPLY) {
                    xEventGroupSetBits(state_machine_get_events(), EVT_WAKE_WORD_DETECTED);
                }
            }
        }
    }
    
    free(raw);
    vTaskDelete(NULL);
}

extern "C" void afe_service_init(void) {
    ESP_LOGI(TAG, "Initializing AFE service");

    models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed. Make sure model partition exists.");
        // We can continue, maybe AFE can work without models if just NS is needed? 
        // But esp_afe_config_init will probably fail without it or with it.
    }

    char *wn_name = NULL;
    if (models) {
        wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
        if (wn_name) {
            ESP_LOGI(TAG, "WakeNet model found: %s", wn_name);
        }
    }

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (!afe_config) {
        ESP_LOGE(TAG, "Failed to create AFE configuration");
        return;
    }

    afe_config->ns_init = true;
    afe_config->aec_init = false;
    afe_config->wakenet_init = true;
    if (wn_name) {
        afe_config->wakenet_model_name = wn_name;
    }
    afe_config->vad_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface = esp_afe_handle_from_config(afe_config);
    if (afe_iface) {
        afe_data = afe_iface->create_from_config(afe_config);
    }
    
    afe_config_free(afe_config);

    if (afe_iface == NULL || afe_data == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE instance");
        return;
    }

    ESP_LOGI(TAG, "AFE service initialized successfully");
}

extern "C" void afe_service_start(void) {
    if (s_running || afe_data == NULL) return;
    s_running = true;
    // Pinned to core 1 maybe? or core 0. Core 0 is audio.
    xTaskCreatePinnedToCore(afe_task, "afe_task", 8192, NULL, configMAX_PRIORITIES - 1, &s_afe_task_handle, 0);
}

extern "C" void afe_service_stop(void) {
    s_running = false;
}

extern "C" bool afe_service_is_wake_word_detected(void) {
    bool ret = s_wake_word_detected;
    s_wake_word_detected = false;
    return ret;
}
