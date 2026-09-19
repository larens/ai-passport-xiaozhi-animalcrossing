#ifndef BACKGROUND_MUSIC_PLAYER_H_
#define BACKGROUND_MUSIC_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string_view>

#include "audio_service.h"

// Loops an embedded Ogg/Opus clip as background music on its own FreeRTOS task.
//
// The clip's gain is baked in offline (50%), so playback never touches the
// global hardware volume and therefore never changes reply loudness. The player
// yields cooperatively: while disabled it stops feeding the decode queue, so
// speech replies and prompts fully own the single audio output. When re-enabled
// it restarts the clip from the beginning and loops forever.
class BackgroundMusicPlayer {
public:
    explicit BackgroundMusicPlayer(AudioService& audio_service);
    ~BackgroundMusicPlayer();

    // Provide the embedded Ogg/Opus data and start the worker task. Safe to call
    // once after AudioService::Start().
    void Start(std::string_view ogg);

    // Request the worker task to stop and join.
    void Stop();

    // Enable/disable playback. Disabling makes the player yield the audio output
    // (used to step aside for speech/replies); enabling resumes looping.
    void SetEnabled(bool enabled);

private:
    static void WorkerEntry(void* arg);
    void WorkerTask();

    AudioService& audio_service_;
    std::string_view ogg_;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> stop_{false};
};

#endif  // BACKGROUND_MUSIC_PLAYER_H_
