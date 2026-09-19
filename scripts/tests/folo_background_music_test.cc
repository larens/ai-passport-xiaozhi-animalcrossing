#include "audio/background_music.h"

#include <cassert>
#include <iostream>

int main() {
    // Background music is audible only while idle.
    assert(BackgroundMusic::ShouldPlay(kDeviceStateIdle));

    // Every speech / capture / full-screen-text state must yield.
    for (auto state : {kDeviceStateUnknown, kDeviceStateStarting, kDeviceStateWifiConfiguring,
                       kDeviceStateConnecting, kDeviceStateListening, kDeviceStateSpeaking,
                       kDeviceStateNotifying, kDeviceStateUpgrading, kDeviceStateActivating,
                       kDeviceStateAudioTesting, kDeviceStateFatalError}) {
        assert(!BackgroundMusic::ShouldPlay(state));
    }

    // Update() reports only transitions of the enabled decision.
    BackgroundMusic music;
    assert(!music.enabled());                       // default disabled
    assert(!music.Update(kDeviceStateStarting));    // still disabled, no change
    assert(music.Update(kDeviceStateIdle));         // disabled -> enabled
    assert(music.enabled());
    assert(!music.Update(kDeviceStateIdle));        // stays enabled, no change
    assert(music.Update(kDeviceStateSpeaking));     // enabled -> disabled (yield)
    assert(!music.enabled());
    assert(!music.Update(kDeviceStateListening));   // stays disabled, no change
    assert(music.Update(kDeviceStateIdle));         // disabled -> enabled (resume)
    assert(music.enabled());

    // A full reply cycle: idle -> connecting -> listening -> speaking -> idle.
    assert(music.Update(kDeviceStateConnecting));   // yield on connect
    assert(!music.enabled());
    assert(!music.Update(kDeviceStateListening));
    assert(!music.Update(kDeviceStateSpeaking));
    assert(music.Update(kDeviceStateIdle));         // resume when the reply ends
    assert(music.enabled());

    std::cout << "Background music policy: idle-only playback and yield "
                 "transitions across a full reply cycle passed.\n";
}
