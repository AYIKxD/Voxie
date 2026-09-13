#include "kws_engine.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "audio/audio_service.h"
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
#endif

static const char* TAG = "KWS_ENGINE";

#define FEATURE_QUEUE_SIZE 10
static QueueHandle_t feature_queue = NULL;

extern const uint8_t model_data_tflite[] asm("_binary_model_data_tflite_start");
extern const uint8_t model_data_tflite_end[] asm("_binary_model_data_tflite_end");

#ifdef VOXIE_HAS_TFLITE
constexpr int kTensorArenaSize = 64 * 1024;
constexpr int kMaxResourceVariables = 8;
static uint8_t tensor_arena[kTensorArenaSize];
static const tflite::Model* model = nullptr;
static tflite::MicroAllocator* allocator = nullptr;
static tflite::MicroResourceVariables* resource_variables = nullptr;
static tflite::MicroInterpreter* interpreter = nullptr;
static TfLiteTensor* input_tensor = nullptr;
static TfLiteTensor* output_tensor = nullptr;
#endif

static TaskHandle_t feature_task_handle = NULL;
static TaskHandle_t kws_task_handle = NULL;
static volatile bool kws_running = false;

/**
 * @brief Simplified feature extraction stub
 * In a full implementation, this uses a micro_speech frontend or similar to compute
 * mel-filterbank energies from raw audio samples.
 */
static void compute_features(const int16_t* audio_data, size_t num_samples, float* features) {
    // Basic stub: populate with dummy feature data.
    // Replace with proper FFT -> Mel Filterbank -> Log later.
    for (int i = 0; i < KWS_FEATURE_SIZE; i++) {
        features[i] = 0.0f;
    }
}

/**
 * @brief Feature Task (Core 0)
 * Reads microphone samples at specified stride, computes features,
 * and passes them to the inference task.
 */
static void feature_task(void* arg) {
    const size_t samples_per_frame = (MIC_SAMPLE_RATE * KWS_FEATURE_STRIDE_MS) / 1000;
    int16_t* audio_buffer = (int16_t*)malloc(samples_per_frame * sizeof(int16_t));
    float* feature_buffer = (float*)malloc(KWS_FEATURE_SIZE * sizeof(float));

    if (!audio_buffer || !feature_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for feature task");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Feature task started");

    while (kws_running) {
        // Read samples from audio service
        size_t samples_read = audio_service_read_mic(audio_buffer, samples_per_frame, 100);
        
        if (samples_read == samples_per_frame) {
            compute_features(audio_buffer, samples_per_frame, feature_buffer);
            xQueueSend(feature_queue, feature_buffer, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5)); // yield on underflow or stream end
        }
    }

    free(audio_buffer);
    free(feature_buffer);
    vTaskDelete(NULL);
}

/**
 * @brief KWS Task (Core 0)
 * Runs TFLite inference on extracted features and applies sliding window detection.
 */
static void kws_task(void* arg) {
    float feature_frame[KWS_FEATURE_SIZE];
    int detection_count = 0;

#ifdef VOXIE_HAS_TFLITE
    // microWakeWord streaming models consume a sliding window of the most
    // recent feature slices, e.g. an input shape of [1, 3, 40].
    static int8_t feature_window[KWS_STREAMING_SLICES][KWS_FEATURE_SIZE];
    int window_fill = 0;
#endif

    ESP_LOGI(TAG, "KWS task started");

    while (kws_running) {
        if (xQueueReceive(feature_queue, feature_frame, portMAX_DELAY) == pdTRUE) {
            float detection_prob = 0.0f;

#ifdef VOXIE_HAS_TFLITE
            if (input_tensor == nullptr || output_tensor == nullptr) {
                continue;
            }

            const float in_scale = input_tensor->params.scale;
            const int in_zero = input_tensor->params.zero_point;

            // Shift the window left by one slice and append the newest frame.
            for (int s = 0; s < KWS_STREAMING_SLICES - 1; s++) {
                memcpy(feature_window[s], feature_window[s + 1], KWS_FEATURE_SIZE);
            }
            for (int i = 0; i < KWS_FEATURE_SIZE; i++) {
                feature_window[KWS_STREAMING_SLICES - 1][i] =
                    (int8_t)(feature_frame[i] / in_scale + in_zero);
            }

            if (window_fill < KWS_STREAMING_SLICES) {
                window_fill++;
                continue;  // need a full window before inferring
            }

            if (input_tensor->type == kTfLiteInt8) {
                memcpy(input_tensor->data.int8, feature_window,
                       KWS_STREAMING_SLICES * KWS_FEATURE_SIZE);
            }

            TfLiteStatus invoke_status = interpreter->Invoke();
            if (invoke_status == kTfLiteOk) {
                if (output_tensor->type == kTfLiteUInt8) {
                    detection_prob = (output_tensor->data.uint8[0] - output_tensor->params.zero_point) * output_tensor->params.scale;
                } else if (output_tensor->type == kTfLiteInt8) {
                    detection_prob = (output_tensor->data.int8[0] - output_tensor->params.zero_point) * output_tensor->params.scale;
                } else if (output_tensor->type == kTfLiteFloat32) {
                    detection_prob = output_tensor->data.f[0];
                }
            } else {
                ESP_LOGE(TAG, "TFLite invoke failed");
            }
#else
            // Stub probability - no real detection unless triggered manually
            detection_prob = 0.0f;
#endif

            // Sliding window threshold logic
            if (detection_prob >= KWS_DETECTION_THRESHOLD) {
                detection_count++;
                if (detection_count >= KWS_SMOOTHING_WINDOW) {
                    ESP_LOGI(TAG, "Wake word detected! Prob: %.2f", (double)detection_prob);
                    // Fire the wake-word event
                    xEventGroupSetBits(state_machine_get_events(), EVT_WAKE_WORD_DETECTED);
                    detection_count = 0; // reset to avoid continuous triggering
                }
            } else {
                detection_count = 0;
            }
        }
    }
    
    vTaskDelete(NULL);
}

extern "C" void kws_engine_init(void) {
    ESP_LOGI(TAG, "Initializing KWS Engine...");

    feature_queue = xQueueCreate(FEATURE_QUEUE_SIZE, KWS_FEATURE_SIZE * sizeof(float));
    if (!feature_queue) {
        ESP_LOGE(TAG, "Failed to create feature queue");
        return;
    }

#ifdef VOXIE_HAS_TFLITE
    tflite::InitializeTarget();
    model = tflite::GetModel(model_data_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model provided is schema version %d not equal to supported version %d.",
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

    // microWakeWord streaming models keep state in TFLM resource variables
    // (VAR_HANDLE / READ_VARIABLE / ASSIGN_VARIABLE), which must be supplied
    // to the interpreter explicitly.
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
#else
    ESP_LOGW(TAG, "TFLite not enabled. KWS will run in stub mode.");
#endif

    ESP_LOGI(TAG, "KWS Engine initialized");
}

extern "C" void kws_engine_start(void) {
    if (kws_running) return;
    kws_running = true;

    // Create tasks on Core 0 (highest priority logic for audio pipeline)
    xTaskCreatePinnedToCore(feature_task, "feature_task", STACK_FEATURE, NULL, configMAX_PRIORITIES - 1, &feature_task_handle, 0);
    xTaskCreatePinnedToCore(kws_task, "kws_task", STACK_KWS, NULL, configMAX_PRIORITIES - 1, &kws_task_handle, 0);
    
    ESP_LOGI(TAG, "KWS Engine tasks started");
}

extern "C" void kws_engine_stop(void) {
    kws_running = false;
    ESP_LOGI(TAG, "KWS Engine stopping...");
}
