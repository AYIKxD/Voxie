#include "kws_engine.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "audio/audio_service.h"
#include "audio/stream_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

#ifdef VOXIE_HAS_TFLITE
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// The int8 micro_speech audio preprocessor model (same 40-value mel-filterbank
// front end used by microWakeWord during training).
#include "audio_preprocessor_int8_model_data.h"
#endif

static const char* TAG = "KWS_ENGINE";

#define FEATURE_QUEUE_SIZE 10
static QueueHandle_t feature_queue = NULL;

extern const uint8_t model_data_tflite[] asm("_binary_model_data_tflite_start");
extern const uint8_t model_data_tflite_end[] asm("_binary_model_data_tflite_end");

#ifdef VOXIE_HAS_TFLITE
// --- Wake-word (streaming) model ---
constexpr int kTensorArenaSize = 64 * 1024;
constexpr int kMaxResourceVariables = 8;
static uint8_t tensor_arena[kTensorArenaSize];
static const tflite::Model* model = nullptr;
static tflite::MicroAllocator* allocator = nullptr;
static tflite::MicroResourceVariables* resource_variables = nullptr;
static tflite::MicroInterpreter* interpreter = nullptr;
static TfLiteTensor* input_tensor = nullptr;
static TfLiteTensor* output_tensor = nullptr;

// --- Audio preprocessor (feature extraction) model ---
constexpr int kPreprocArenaSize = 16 * 1024;
static uint8_t preproc_arena[kPreprocArenaSize];
static tflite::MicroInterpreter* preproc_interpreter = nullptr;
static TfLiteTensor* preproc_input = nullptr;
static TfLiteTensor* preproc_output = nullptr;

constexpr int kPreprocWindowSamples = 480;  // 30 ms @ 16 kHz
#endif

static TaskHandle_t feature_task_handle = NULL;
static TaskHandle_t kws_task_handle = NULL;
static volatile bool kws_running = false;

// ---------------------------------------------------------------------------
// Feature task (Core 0): 10 ms mic chunks -> 30 ms window -> 40 int8 features
// ---------------------------------------------------------------------------
static void feature_task(void* arg) {
    const size_t samples_per_frame =
        (MIC_SAMPLE_RATE * KWS_FEATURE_STRIDE_MS) / 1000;  // 160 samples (10 ms)
    int16_t chunk[160];
    int8_t features[KWS_FEATURE_SIZE];

#ifdef VOXIE_HAS_TFLITE
    static int16_t audio_window[kPreprocWindowSamples];
    memset(audio_window, 0, sizeof(audio_window));
#endif

    ESP_LOGI(TAG, "Feature task started");

    while (kws_running) {
        size_t samples_read = audio_service_read_mic(chunk, samples_per_frame, 100);
        if (samples_read == 0) {
            continue;
        }
        // Feed the streaming/pre-roll pipeline (non-blocking).
        stream_service_push_mic(chunk, samples_read);
        if (samples_read != samples_per_frame) {
            continue;
        }

#ifdef VOXIE_HAS_TFLITE
        if (preproc_interpreter == nullptr) {
            continue;
        }

        // Slide the 30 ms window left by one stride and append the new chunk.
        memmove(audio_window, audio_window + samples_per_frame,
                (kPreprocWindowSamples - samples_per_frame) * sizeof(int16_t));
        memcpy(audio_window + (kPreprocWindowSamples - samples_per_frame),
               chunk, samples_per_frame * sizeof(int16_t));

        memcpy(tflite::GetTensorData<int16_t>(preproc_input), audio_window,
               kPreprocWindowSamples * sizeof(int16_t));

        if (preproc_interpreter->Invoke() == kTfLiteOk) {
            memcpy(features, tflite::GetTensorData<int8_t>(preproc_output),
                   KWS_FEATURE_SIZE);
            xQueueSend(feature_queue, features, portMAX_DELAY);
        }
#else
        memset(features, 0, sizeof(features));
        xQueueSend(feature_queue, features, portMAX_DELAY);
#endif
    }

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// KWS task (Core 0): rolling [3 x 40] int8 window -> wake-word probability
// ---------------------------------------------------------------------------
static void kws_task(void* arg) {
    int8_t feature_frame[KWS_FEATURE_SIZE];
    int detection_count = 0;
    float prob_history[KWS_AVG_WINDOW];
    int hist_count = 0;
    int hist_idx = 0;
    float s_peak_prob = 0.0f;
    int s_status_counter = 0;

    memset(prob_history, 0, sizeof(prob_history));

#ifdef VOXIE_HAS_TFLITE
    static int8_t feature_window[KWS_STREAMING_SLICES][KWS_FEATURE_SIZE];
    memset(feature_window, 0, sizeof(feature_window));
    int window_fill = 0;
#endif

    ESP_LOGI(TAG, "KWS task started");

    while (kws_running) {
        if (xQueueReceive(feature_queue, feature_frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        float detection_prob = 0.0f;

#ifdef VOXIE_HAS_TFLITE
        if (input_tensor == nullptr || output_tensor == nullptr) {
            continue;
        }

        // Shift the streaming window and append the newest feature slice.
        for (int s = 0; s < KWS_STREAMING_SLICES - 1; s++) {
            memcpy(feature_window[s], feature_window[s + 1], KWS_FEATURE_SIZE);
        }
        memcpy(feature_window[KWS_STREAMING_SLICES - 1], feature_frame,
               KWS_FEATURE_SIZE);

        if (window_fill < KWS_STREAMING_SLICES) {
            window_fill++;
            continue;  // need a full window before inferring
        }

        memcpy(tflite::GetTensorData<int8_t>(input_tensor), feature_window,
               KWS_STREAMING_SLICES * KWS_FEATURE_SIZE);

        if (interpreter->Invoke() == kTfLiteOk) {
            if (output_tensor->type == kTfLiteUInt8) {
                detection_prob =
                    (tflite::GetTensorData<uint8_t>(output_tensor)[0] -
                     output_tensor->params.zero_point) * output_tensor->params.scale;
            } else if (output_tensor->type == kTfLiteInt8) {
                detection_prob =
                    (tflite::GetTensorData<int8_t>(output_tensor)[0] -
                     output_tensor->params.zero_point) * output_tensor->params.scale;
            } else if (output_tensor->type == kTfLiteFloat32) {
                detection_prob = tflite::GetTensorData<float>(output_tensor)[0];
            }
        } else {
            ESP_LOGE(TAG, "TFLite invoke failed");
        }
#endif

        // Moving average over the last KWS_AVG_WINDOW frame probabilities
        // (matches the sliding window used by the training-time evaluation).
        prob_history[hist_idx] = detection_prob;
        hist_idx = (hist_idx + 1) % KWS_AVG_WINDOW;
        if (hist_count < KWS_AVG_WINDOW) {
            hist_count++;
        }

        float sum = 0.0f;
        for (int i = 0; i < hist_count; i++) {
            sum += prob_history[i];
        }
        float avg_prob = sum / hist_count;

        // Periodic visibility into the live probability (validates the feature
        // pipeline without spamming the log).
        if (avg_prob > s_peak_prob) {
            s_peak_prob = avg_prob;
        }
        if (++s_status_counter >= 500) {  // ~5 s at 10 ms stride
            ESP_LOGI(TAG, "listening: peak prob %.2f", (double)s_peak_prob);
            s_peak_prob = 0.0f;
            s_status_counter = 0;
        }

        if (avg_prob >= KWS_DETECTION_THRESHOLD) {
            detection_count++;
            if (detection_count >= KWS_SMOOTHING_WINDOW) {
                ESP_LOGI(TAG, "Wake word detected! Prob: %.2f", (double)avg_prob);
                xEventGroupSetBits(state_machine_get_events(), EVT_WAKE_WORD_DETECTED);
                detection_count = 0;
                hist_count = 0;  // reset averaging after a detection
                memset(prob_history, 0, sizeof(prob_history));
            }
        } else {
            detection_count = 0;
        }
    }

    vTaskDelete(NULL);
}

extern "C" void kws_engine_init(void) {
    ESP_LOGI(TAG, "Initializing KWS Engine...");

    feature_queue = xQueueCreate(FEATURE_QUEUE_SIZE, KWS_FEATURE_SIZE);
    if (!feature_queue) {
        ESP_LOGE(TAG, "Failed to create feature queue");
        return;
    }

#ifdef VOXIE_HAS_TFLITE
    tflite::InitializeTarget();

    // --- Feature extractor interpreter ---
    const tflite::Model* preproc_model = tflite::GetModel(g_audio_preprocessor_int8_tflite);
    if (preproc_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Preprocessor model schema mismatch");
        return;
    }

    static tflite::MicroMutableOpResolver<18> preproc_resolver;
    preproc_resolver.AddReshape();
    preproc_resolver.AddCast();
    preproc_resolver.AddStridedSlice();
    preproc_resolver.AddConcatenation();
    preproc_resolver.AddMul();
    preproc_resolver.AddAdd();
    preproc_resolver.AddDiv();
    preproc_resolver.AddMinimum();
    preproc_resolver.AddMaximum();
    preproc_resolver.AddWindow();
    preproc_resolver.AddFftAutoScale();
    preproc_resolver.AddRfft();
    preproc_resolver.AddEnergy();
    preproc_resolver.AddFilterBank();
    preproc_resolver.AddFilterBankSquareRoot();
    preproc_resolver.AddFilterBankSpectralSubtraction();
    preproc_resolver.AddPCAN();
    preproc_resolver.AddFilterBankLog();

    static tflite::MicroInterpreter preproc_static_interpreter(
        preproc_model, preproc_resolver, preproc_arena, kPreprocArenaSize);
    preproc_interpreter = &preproc_static_interpreter;

    if (preproc_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "Preprocessor AllocateTensors() failed");
        preproc_interpreter = nullptr;
        return;
    }
    preproc_input = preproc_interpreter->input(0);
    preproc_output = preproc_interpreter->output(0);
    ESP_LOGI(TAG, "Preprocessor ready (arena used %u bytes)",
             (unsigned)preproc_interpreter->arena_used_bytes());

    // --- Wake-word model interpreter ---
    model = tflite::GetModel(model_data_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema version %d != supported %d",
                 (int)model->version(), (int)TFLITE_SCHEMA_VERSION);
        return;
    }

    // Exact op set used by the trained microWakeWord streaming model.
    static tflite::MicroMutableOpResolver<13> micro_op_resolver;
    micro_op_resolver.AddConv2D();
    micro_op_resolver.AddDepthwiseConv2D();
    micro_op_resolver.AddFullyConnected();
    micro_op_resolver.AddReshape();
    micro_op_resolver.AddLogistic();
    micro_op_resolver.AddConcatenation();
    micro_op_resolver.AddSplitV();
    micro_op_resolver.AddStridedSlice();
    micro_op_resolver.AddQuantize();
    micro_op_resolver.AddCallOnce();
    micro_op_resolver.AddVarHandle();
    micro_op_resolver.AddReadVariable();
    micro_op_resolver.AddAssignVariable();

    // microWakeWord streaming models keep state in TFLM resource variables,
    // which must be supplied to the interpreter explicitly.
    allocator = tflite::MicroAllocator::Create(tensor_arena, kTensorArenaSize);
    if (allocator == nullptr) {
        ESP_LOGE(TAG, "Failed to create TFLM allocator");
        return;
    }
    resource_variables =
        tflite::MicroResourceVariables::Create(allocator, kMaxResourceVariables);
    if (resource_variables == nullptr) {
        ESP_LOGE(TAG, "Failed to create TFLM resource variables");
        return;
    }

    static tflite::MicroInterpreter static_interpreter(
        model, micro_op_resolver, allocator, resource_variables);
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors() failed");
        return;
    }

    input_tensor = interpreter->input(0);
    output_tensor = interpreter->output(0);
    ESP_LOGI(TAG, "Wake-word model ready (arena used %u bytes)",
             (unsigned)interpreter->arena_used_bytes());
#else
    ESP_LOGW(TAG, "TFLite not enabled. KWS will run in stub mode.");
#endif

    ESP_LOGI(TAG, "KWS Engine initialized");
}

extern "C" void kws_engine_start(void) {
    if (kws_running) return;
    kws_running = true;

    // Audio-critical path pinned to Core 0.
    xTaskCreatePinnedToCore(feature_task, "feature_task", STACK_FEATURE, NULL,
                            configMAX_PRIORITIES - 1, &feature_task_handle, 0);
    xTaskCreatePinnedToCore(kws_task, "kws_task", STACK_KWS, NULL,
                            configMAX_PRIORITIES - 1, &kws_task_handle, 0);

    ESP_LOGI(TAG, "KWS Engine tasks started");
}

extern "C" void kws_engine_stop(void) {
    kws_running = false;
    ESP_LOGI(TAG, "KWS Engine stopping...");
}
