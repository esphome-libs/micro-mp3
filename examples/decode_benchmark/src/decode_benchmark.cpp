// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/* ESP32 MP3 Decode Benchmark
 *
 * Continuously decodes 30-second MP3 audio clips (64kbps, 128kbps, 320kbps stereo)
 * and reports timing statistics.
 *
 * Uses Mp3Decoder to parse and decode the MP3 stream.
 *
 * Demonstrates concurrent decoding with independent decoder instances by testing
 * 1-4 tasks for each clip, with tasks pinned to alternating CPU cores.
 *
 * Each task uses its own Mp3Decoder instance. A single instance is not
 * thread-safe; concurrency comes from running separate instances, one per task.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "micro_mp3/mp3_decoder.h"
#include "test_audio_mp3_128k.h"
#include "test_audio_mp3_320k.h"
#include "test_audio_mp3_64k.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* const TAG = "DECODE_BENCH";

#ifndef DECODE_BENCH_MAX_CONCURRENT_TASKS
#define DECODE_BENCH_MAX_CONCURRENT_TASKS 4
#endif

static const int MAX_CONCURRENT_TASKS = DECODE_BENCH_MAX_CONCURRENT_TASKS;

// Output buffer size: large enough for one MP3 frame
// MPEG1 stereo: 1152 samples/ch * 2 ch * 2 bytes = 4608 bytes
static const size_t PCM_OUTPUT_BUFFER_BYTES = micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES;

// Audio clip descriptor
struct AudioClip {
    const char* name;
    const uint8_t* data;
    size_t len;
};

static const AudioClip AUDIO_CLIPS[] = {
    {"MP3 64kbps (48kHz stereo)", test_audio_mp3_64k, test_audio_mp3_64k_len},
    {"MP3 128kbps (48kHz stereo)", test_audio_mp3_128k, test_audio_mp3_128k_len},
    {"MP3 320kbps (48kHz stereo)", test_audio_mp3_320k, test_audio_mp3_320k_len},
};
static const int NUM_CLIPS = sizeof(AUDIO_CLIPS) / sizeof(AUDIO_CLIPS[0]);

// Statistics structure for tracking timing data
struct Stats {
    int64_t min_us;
    int64_t max_us;
    int64_t sum_us;
    int64_t sum_sq_us;  // For standard deviation calculation
    size_t count;
    size_t total_samples;  // Total audio samples decoded (per channel)
};

// Results from a decode run
struct DecodeResult {
    Stats frame_stats;
    int64_t total_time_us;
    int64_t setup_time_us;   // Time to parse the stream header (first decode() probe)
    int64_t decode_time_us;  // Time for audio decoding only
    uint32_t sample_rate;
    uint32_t bitrate;
    uint8_t channels;
    int core_id;
    bool success;
    bool footprint_valid;           // True only when measured alone (no concurrent tasks)
    size_t decoder_heap_bytes;      // Total free heap consumed by this decoder once fully allocated
    size_t decoder_internal_bytes;  // Of that, drawn from internal RAM
    size_t decoder_psram_bytes;     // Of that, drawn from PSRAM
};

// Task parameters
struct TaskParams {
    int task_id;
    const AudioClip* clip;
    DecodeResult* result;
    SemaphoreHandle_t done_semaphore;
    int pinned_core;         // -1 for no pinning, 0 or 1 for specific core
    bool measure_footprint;  // Only true for single-task runs (see decode_full_file)
};

// Initialize statistics structure
static void init_stats(Stats* s) {
    s->min_us = INT64_MAX;
    s->max_us = 0;
    s->sum_us = 0;
    s->sum_sq_us = 0;
    s->count = 0;
    s->total_samples = 0;
}

// Update statistics with new timing value and sample count
static void update_stats(Stats* s, int64_t time_us, size_t samples) {
    if (time_us < s->min_us) {
        s->min_us = time_us;
    }

    if (time_us > s->max_us) {
        s->max_us = time_us;
    }
    s->sum_us += time_us;
    s->sum_sq_us += time_us * time_us;
    s->count++;
    s->total_samples += samples;
}

// Log statistics
static void log_stats(const char* prefix, const char* name, Stats* s) {
    if (s->count == 0) {
        ESP_LOGI(TAG, "%s%s: no data", prefix, name);
        return;
    }

    double avg = (double)s->sum_us / s->count;
    double variance = (double)s->sum_sq_us / s->count - avg * avg;
    double stddev = sqrt(variance);

    ESP_LOGI(TAG, "%s%s (us): min=%" PRId64 " max=%" PRId64 " avg=%.1f sd=%.1f (n=%zu)", prefix,
             name, s->min_us, s->max_us, avg, stddev, s->count);
}

// Decode the full test audio clip and return results.
//
// Set measure_footprint only for a solo decode task. The footprint is a free-heap
// delta from the system-wide capability counters, which concurrent tasks would
// corrupt with their own allocations. It is deterministic per stream, so one
// single-task measurement suffices.
static DecodeResult decode_full_file(const AudioClip& clip, bool measure_footprint) {
    DecodeResult result{};
    init_stats(&result.frame_stats);
    result.success = true;
    result.sample_rate = 0;
    result.bitrate = 0;
    result.channels = 0;
    result.setup_time_us = 0;
    result.decode_time_us = 0;
    result.core_id = xPortGetCoreID();
    result.footprint_valid = false;
    result.decoder_heap_bytes = 0;
    result.decoder_internal_bytes = 0;
    result.decoder_psram_bytes = 0;

    // Baseline free heap before any allocation, split by capability, so the
    // post-init delta shows how the footprint (decoder state + PCM buffer) divides
    // between internal RAM and PSRAM.
    size_t internal_before = measure_footprint ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL) : 0;
    size_t psram_before = measure_footprint ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;

    // Create decoder (constructor always succeeds; lazy allocation on first decode)
    micro_mp3::Mp3Decoder decoder;

    // PCM output buffer: heap-allocated to avoid stack overflow in FreeRTOS tasks.
    // Sized once to the MPEG1 stereo worst case; MP3 has a known maximum frame size,
    // so no resize-on-the-fly is needed.
    uint8_t* pcm_buffer = (uint8_t*)heap_caps_malloc(PCM_OUTPUT_BUFFER_BYTES, MALLOC_CAP_DEFAULT);
    if (pcm_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate PCM output buffer");
        result.success = false;
        return result;
    }

    // Input data pointers
    const uint8_t* input_ptr = clip.data;
    size_t input_remaining = clip.len;

    // Start timing
    int64_t iteration_start = esp_timer_get_time();

    // Decode loop
    while (input_remaining > 0) {
        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;

        // Time this decode call
        int64_t frame_start = esp_timer_get_time();

        micro_mp3::Mp3Result decode_result =
            decoder.decode(input_ptr, input_remaining, pcm_buffer, PCM_OUTPUT_BUFFER_BYTES,
                           bytes_consumed, samples_decoded);

        int64_t frame_time = esp_timer_get_time() - frame_start;

        // The first decode() call parses the stream header and returns
        // MP3_STREAM_INFO_READY without emitting audio. Record setup time here, and
        // (solo runs only) capture the footprint now that the decoder is fully
        // allocated: lazy-init decoder state plus the PCM buffer.
        if (decode_result == micro_mp3::MP3_STREAM_INFO_READY) {
            result.setup_time_us = esp_timer_get_time() - iteration_start;

            if (measure_footprint) {
                size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                result.decoder_internal_bytes =
                    internal_before > internal_after ? internal_before - internal_after : 0;
                result.decoder_psram_bytes =
                    psram_before > psram_after ? psram_before - psram_after : 0;
                result.decoder_heap_bytes =
                    result.decoder_internal_bytes + result.decoder_psram_bytes;
                result.footprint_valid = true;
            }
        }

        // Update statistics only when samples were decoded
        // samples_decoded is per-channel; total output = samples_decoded * channels
        if (samples_decoded > 0) {
            update_stats(&result.frame_stats, frame_time, samples_decoded);
        }

        // Check for errors
        if (decode_result < 0) {
            if (decode_result == micro_mp3::MP3_DECODE_ERROR) {
                // Recoverable: the decoder has already skipped the bad frame
                ESP_LOGW(TAG, "Skipping corrupt frame (consumed=%zu, remaining=%zu)",
                         bytes_consumed, input_remaining);
            } else {
                // Fatal error
                ESP_LOGE(TAG, "Fatal decode error: %d (consumed=%zu, remaining=%zu)",
                         (int)decode_result, bytes_consumed, input_remaining);
                result.success = false;
                break;
            }
        }

        // Advance input pointer (always, even on recoverable error)
        if (bytes_consumed > 0) {
            input_ptr += bytes_consumed;
            input_remaining -= bytes_consumed;
        } else if (decode_result < 0) {
            // Fatal error with 0 bytes consumed: stop to avoid infinite loop
            break;
        }

        // Yield to allow other tasks to run (important for concurrent decoding)
        taskYIELD();
    }

    result.total_time_us = esp_timer_get_time() - iteration_start;
    result.decode_time_us = result.total_time_us - result.setup_time_us;
    result.sample_rate = decoder.get_sample_rate();
    result.bitrate = decoder.get_bitrate();
    result.channels = decoder.get_channels();

    heap_caps_free(pcm_buffer);

    return result;
}

// Log decode results with optional prefix
static void log_decode_result(const char* prefix, DecodeResult* result) {
    if (!result->success) {
        ESP_LOGE(TAG, "%sDecode failed", prefix);
        return;
    }

    log_stats(prefix, "Frame", &result->frame_stats);

    // Calculate real-time factor
    // total_samples is per-channel; audio duration uses per-channel sample count / sample_rate
    double audio_duration_us =
        (double)result->frame_stats.total_samples / result->sample_rate * 1000000.0;
    double rtf = (double)result->total_time_us / audio_duration_us;

    double decode_rtf = (double)result->decode_time_us / audio_duration_us;

    ESP_LOGI(TAG,
             "%sTotal: %" PRId64 " ms (setup: %" PRId64 " ms, decode: %" PRId64
             " ms), %.1fs audio, RTF: %.3f (%.1fx), decode RTF: %.3f (%.1fx), "
             "%u Hz, %u ch, %" PRIu32 " kbps, core %d",
             prefix, result->total_time_us / 1000, result->setup_time_us / 1000,
             result->decode_time_us / 1000, audio_duration_us / 1000000.0, rtf, 1.0 / rtf,
             decode_rtf, 1.0 / decode_rtf, result->sample_rate, result->channels, result->bitrate,
             result->core_id);

    // Only logged for single-task runs; under concurrency the measurement is
    // unreliable (see decode_full_file) and is skipped entirely.
    if (result->footprint_valid) {
        ESP_LOGI(TAG,
                 "%sDecoder footprint: %zu bytes (internal: %zu, PSRAM: %zu) (decoder state + PCM "
                 "buffer)",
                 prefix, result->decoder_heap_bytes, result->decoder_internal_bytes,
                 result->decoder_psram_bytes);
    }
}

// FreeRTOS task function for concurrent decoding
static void decode_task(void* params) {
    TaskParams* task_params = (TaskParams*)params;

    ESP_LOGI(TAG, "Task %d starting MP3 decode on core %d...", task_params->task_id,
             xPortGetCoreID());

    // Decode the full file
    *task_params->result = decode_full_file(*task_params->clip, task_params->measure_footprint);

    ESP_LOGI(TAG, "Task %d finished (%" PRId64 " ms)", task_params->task_id,
             task_params->result->total_time_us / 1000);

    // Signal completion
    xSemaphoreGive(task_params->done_semaphore);

    // Delete this task
    vTaskDelete(NULL);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 MP3 Decode Benchmark ===");
    ESP_LOGI(TAG, "Audio: 30s Beethoven Symphony No. 3 (from 1:00), 48kHz stereo");
    ESP_LOGI(TAG, "  MP3 64kbps:  %u bytes", test_audio_mp3_64k_len);
    ESP_LOGI(TAG, "  MP3 128kbps: %u bytes", test_audio_mp3_128k_len);
    ESP_LOGI(TAG, "  MP3 320kbps: %u bytes", test_audio_mp3_320k_len);
    ESP_LOGI(TAG, "Free heap: %zu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Free Internal: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Concurrent decode test: up to %d independent tasks", MAX_CONCURRENT_TASKS);

    // Create semaphore for task synchronization
    SemaphoreHandle_t done_semaphore = xSemaphoreCreateCounting(MAX_CONCURRENT_TASKS, 0);
    if (done_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    uint32_t iteration = 0;

    while (true) {
        iteration++;
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "=== Iteration %" PRIu32 " ===", iteration);

        bool all_success = true;

        for (int clip_idx = 0; clip_idx < NUM_CLIPS; clip_idx++) {
            const AudioClip& clip = AUDIO_CLIPS[clip_idx];

            // Track wall-clock times for each task count
            int64_t times[MAX_CONCURRENT_TASKS] = {0};

            for (int num_tasks = 1; num_tasks <= MAX_CONCURRENT_TASKS; num_tasks++) {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "--- %s - %d concurrent task%s ---", clip.name, num_tasks,
                         num_tasks == 1 ? "" : "s");

                DecodeResult results[MAX_CONCURRENT_TASKS];
                TaskParams params[MAX_CONCURRENT_TASKS];

                // Set up task parameters
                for (int i = 0; i < num_tasks; i++) {
                    params[i].task_id = i;
                    params[i].clip = &clip;
                    params[i].result = &results[i];
                    params[i].done_semaphore = done_semaphore;
                    params[i].pinned_core = i % 2;
                    // Only trust the heap-diff footprint when this decoder runs
                    // alone; concurrent tasks share the global free-heap counters.
                    params[i].measure_footprint = (num_tasks == 1);
                }

                int64_t start_time = esp_timer_get_time();

                // Create all tasks pinned to alternating cores
                int tasks_created = 0;
                for (int i = 0; i < num_tasks; i++) {
                    char task_name[16];
                    snprintf(task_name, sizeof(task_name), "decode_%d", i);

                    BaseType_t ret =
                        xTaskCreatePinnedToCore(decode_task, task_name, 5192, &params[i],
                                                1,  // Priority
                                                NULL,
                                                i % 2  // Core ID: alternates 0, 1, 0, 1
                        );

                    if (ret == pdPASS) {
                        tasks_created++;
                    } else {
                        ESP_LOGE(TAG, "Failed to create task %d", i);
                    }
                }

                // Wait for all successfully created tasks to complete
                for (int i = 0; i < tasks_created; i++) {
                    xSemaphoreTake(done_semaphore, portMAX_DELAY);
                }

                times[num_tasks - 1] = esp_timer_get_time() - start_time;

                // Log per-task results
                for (int i = 0; i < tasks_created; i++) {
                    char prefix[16];
                    snprintf(prefix, sizeof(prefix), "Task %d: ", i);
                    log_decode_result(prefix, &results[i]);
                    all_success = all_success && results[i].success;
                }
            }

            // Per-clip summary
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "--- Summary (%s) ---", clip.name);
            for (int i = 0; i < MAX_CONCURRENT_TASKS; i++) {
                ESP_LOGI(TAG, "  %d task%s  %6" PRId64 " ms", i + 1,
                         i == 0 ? ": " : "s:", times[i] / 1000);
            }
        }

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "All decodes successful: %s", all_success ? "YES" : "NO");
        ESP_LOGI(TAG, "Free heap: %zu bytes", esp_get_free_heap_size());

        // Low-water marks since boot. They survive task teardown, so they capture
        // the trough during peak concurrency (4 decoders + buffers + stacks live at
        // once), not the current idle state.
        ESP_LOGI(TAG, "Min free heap ever:     %zu bytes", esp_get_minimum_free_heap_size());
        ESP_LOGI(TAG, "Min free internal ever: %zu bytes",
                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "Min free PSRAM ever:    %zu bytes",
                 heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGI(TAG, "---");

        // Small delay between iterations
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
