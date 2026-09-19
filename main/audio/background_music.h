#pragma once

#include "device_state.h"

// Pure, host-testable policy that decides whether the looping background music
// should currently be audible. It carries no ESP/audio dependencies so it can
// be unit-tested on the host.
//
// Rule: background music plays only while the device is idle. During any state
// that produces or captures speech (listening/speaking/notifying) or that owns
// the screen for full-screen text (connecting/provisioning/activating/
// upgrading/fatal error), it must yield so it never competes with a reply.
class BackgroundMusic {
public:
    // Returns whether background music should be enabled for the given state.
    static constexpr bool ShouldPlay(DeviceState state) {
        return state == kDeviceStateIdle;
    }

    // Update the tracked state and report whether the enabled decision changed,
    // so callers only act on transitions.
    bool Update(DeviceState state) {
        const bool enabled = ShouldPlay(state);
        const bool changed = enabled != enabled_;
        enabled_ = enabled;
        return changed;
    }

    bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
};
