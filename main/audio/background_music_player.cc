#include "background_music_player.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <esp_log.h>

#include "ogg_demuxer.h"

namespace {
const char* TAG = "BgMusic";
constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = 1;  // Below opus_codec (2) and output (4).
// Feed the demuxer in small chunks so the yield/stop flags are checked often.
constexpr size_t kChunkSize = 512;
constexpr TickType_t kIdlePollDelay = pdMS_TO_TICKS(200);
}  // namespace

BackgroundMusicPlayer::BackgroundMusicPlayer(AudioService& audio_service)
    : audio_service_(audio_service) {}

BackgroundMusicPlayer::~BackgroundMusicPlayer() { Stop(); }

void BackgroundMusicPlayer::Start(std::string_view ogg) {
    if (task_handle_ != nullptr || ogg.empty()) {
        return;
    }
    ogg_ = ogg;
    stop_.store(false);
    xTaskCreate(&BackgroundMusicPlayer::WorkerEntry, "bg_music", kTaskStackSize, this,
                kTaskPriority, &task_handle_);
}

void BackgroundMusicPlayer::Stop() {
    if (task_handle_ == nullptr) {
        return;
    }
    stop_.store(true);
    enabled_.store(false);
    // The worker deletes itself; wait for it to observe the stop flag.
    while (task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void BackgroundMusicPlayer::SetEnabled(bool enabled) { enabled_.store(enabled); }

void BackgroundMusicPlayer::WorkerEntry(void* arg) {
    auto self = static_cast<BackgroundMusicPlayer*>(arg);
    self->WorkerTask();
    self->task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void BackgroundMusicPlayer::WorkerTask() {
    const auto* buffer = reinterpret_cast<const uint8_t*>(ogg_.data());
    const size_t total = ogg_.size();

    OggDemuxer demuxer;
    // Push one decoded packet into the shared decode queue, honoring yield/stop.
    demuxer.OnPacket([this](const uint8_t* data, int sample_rate, int frame_duration_ms,
                            size_t size) {
        if (stop_.load() || !enabled_.load()) {
            return;  // Drop packets while yielding; the outer loop will restart.
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration_ms;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        // wait=true blocks until the queue has room; because we only feed while
        // enabled and idle, speech never has to wait behind a full backlog.
        audio_service_.PushPacketToDecodeQueue(std::move(packet), true);
    });

    while (!stop_.load()) {
        // Wait until enabled and the output is idle so we never compete with a
        // reply or prompt for the single audio output.
        if (!enabled_.load() || !audio_service_.IsPlaybackIdle()) {
            vTaskDelay(kIdlePollDelay);
            continue;
        }

        // Stream the whole clip once, in small chunks, then loop.
        demuxer.Reset();
        size_t offset = 0;
        while (offset < total && !stop_.load() && enabled_.load()) {
            const size_t chunk = std::min(kChunkSize, total - offset);
            demuxer.Process(buffer + offset, chunk);
            if (demuxer.HasError()) {
                ESP_LOGW(TAG, "Ogg demux error; restarting clip");
                break;
            }
            offset += chunk;
        }
        // Small gap between loops; also lets a yield request settle.
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
