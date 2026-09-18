#include "audio/audio.h"

#include <cmath>
#include <cstring>

#include "board/board.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

namespace wc::audio {

namespace {

constexpr int SAMPLE_RATE = 22050;
constexpr int CHUNK = 220;      // 10 ms: short enough that sounds start promptly
constexpr int VOICES = 6;
constexpr const char *TAG = "audio";

constexpr float kLevelGain[LEVELS] = {0.0f, 0.25f, 0.55f, 1.0f};

esp_codec_dev_handle_t s_codec;
QueueHandle_t s_queue;
int s_volume = 2;
bool s_ready;

struct Voice {
    bool active = false;
    Wave wave = Wave::Square;
    float phase = 0;
    float f0 = 0, f1 = 0;
    float gain = 0;
    int samples_total = 0, samples_done = 0;
    int delay_samples = 0;
};

Voice s_voices[VOICES];

float waveSample(Wave w, float phase, uint32_t &noise)
{
    switch (w) {
    case Wave::Square: return phase < 0.5f ? 1.0f : -1.0f;
    case Wave::Triangle: return 4.0f * std::fabs(phase - 0.5f) - 1.0f;
    case Wave::Noise:
    default:
        noise = noise * 1664525u + 1013904223u;
        return float(int32_t(noise >> 16) & 0xFFFF) / 32768.0f - 1.0f;
    }
}

void mixerTask(void *)
{
    static int16_t out[CHUNK];
    uint32_t noise = esp_random();

    for (;;) {
        // Pick up anything the game asked for since the last chunk.
        Tone t;
        while (xQueueReceive(s_queue, &t, 0) == pdTRUE) {
            Voice *slot = nullptr;
            for (auto &v : s_voices) {
                if (!v.active) {
                    slot = &v;
                    break;
                }
            }
            if (!slot) slot = &s_voices[0];   // all busy: steal the oldest slot
            slot->active = true;
            slot->wave = t.wave;
            slot->phase = 0;
            slot->f0 = t.f0;
            slot->f1 = t.f1 > 0 ? t.f1 : t.f0;
            slot->gain = t.volume;
            slot->samples_total = std::max(1, t.ms * SAMPLE_RATE / 1000);
            slot->samples_done = 0;
            slot->delay_samples = t.delay_ms * SAMPLE_RATE / 1000;
        }

        const float master = kLevelGain[s_volume] * 0.8f;
        bool any = false;
        for (int i = 0; i < CHUNK; i++) {
            float sample = 0;
            for (auto &v : s_voices) {
                if (!v.active) continue;
                any = true;
                if (v.delay_samples > 0) {
                    v.delay_samples--;
                    continue;
                }
                const float t01 = float(v.samples_done) / float(v.samples_total);
                const float freq = v.f0 + (v.f1 - v.f0) * t01;
                // Quick attack, smooth decay: percussive without clicking.
                const float env = t01 < 0.02f ? t01 / 0.02f : (1.0f - t01) * (1.0f - t01);
                sample += waveSample(v.wave, v.phase, noise) * env * v.gain;
                v.phase += freq / SAMPLE_RATE;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                if (++v.samples_done >= v.samples_total) v.active = false;
            }
            sample *= master;
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            out[i] = int16_t(sample * 26000);
        }

        if (any || s_volume > 0) {
            esp_codec_dev_write(s_codec, out, sizeof(out));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

bool startCodec()
{
    ESP_LOGI(TAG, "starting ES8311 (free internal %u bytes)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, &tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = PIN_I2S_MCLK,
                .bclk = PIN_I2S_BCLK,
                .ws = PIN_I2S_WS,
                .dout = PIN_I2S_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {false, false, false},
            },
    };
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(tx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = nullptr, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = board_i2c()};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!data_if || !ctrl_if) return false;

    es8311_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if = ctrl_if;
    es_cfg.gpio_if = audio_codec_new_gpio();
    es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es_cfg.pa_pin = PIN_PA_EN;
    es_cfg.use_mclk = true;
    es_cfg.hw_gain.pa_voltage = 5.0f;
    es_cfg.hw_gain.codec_dac_voltage = 3.3f;
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) return false;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if};
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) return false;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = SAMPLE_RATE, .mclk_multiple = 0};
    if (esp_codec_dev_open(s_codec, &fs) != ESP_OK) return false;
    esp_codec_dev_set_out_vol(s_codec, 75);
    return true;
}

}  // namespace

int volume() { return s_volume; }

void setVolume(int level)
{
    s_volume = level % LEVELS;
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "volume", uint8_t(s_volume));
        nvs_commit(h);
        nvs_close(h);
    }
}

bool init()
{
    if (s_ready) return true;
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "volume", &v) == ESP_OK && v < LEVELS) s_volume = v;
        nvs_close(h);
    }

    if (!startCodec()) {
        ESP_LOGE(TAG, "no codec; sound off");
        return false;
    }
    s_queue = xQueueCreate(16, sizeof(Tone));
    xTaskCreatePinnedToCore(mixerTask, "audio", 6144, nullptr, configMAX_PRIORITIES - 6, nullptr, 0);
    s_ready = true;
    ESP_LOGI(TAG, "ES8311 ready, volume %s", LEVEL_NAMES[s_volume]);
    return true;
}

void play(const Tone &t)
{
    if (!s_ready || s_volume == 0) return;
    xQueueSend(s_queue, &t, 0);
}

void play(const Tone *tones, int count)
{
    for (int i = 0; i < count; i++) play(tones[i]);
}

void stopAll()
{
    for (auto &v : s_voices) v.active = false;
}

}  // namespace wc::audio
