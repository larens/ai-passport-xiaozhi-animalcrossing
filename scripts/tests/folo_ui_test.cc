#include "screen_idle.h"

#include <cassert>
#include <iostream>

int main() {
    constexpr auto timeout = ScreenIdle::kTimeoutUs;
    ScreenIdle idle;
    idle.Activity(1000);
    assert(!idle.Tick(1000 + timeout - 1, kDeviceStateIdle, false));
    assert(idle.Tick(1000 + timeout, kDeviceStateIdle, false));
    assert(idle.Activity(1000 + timeout + 1));
    assert(!idle.Activity(1000 + timeout + 2));

    for (auto state : {kDeviceStateIdle, kDeviceStateListening, kDeviceStateConnecting}) {
        idle.Activity(0);
        assert(idle.Tick(timeout, state, false));
    }
    for (auto state : {kDeviceStateUnknown, kDeviceStateStarting, kDeviceStateWifiConfiguring,
                       kDeviceStateSpeaking, kDeviceStateNotifying, kDeviceStateUpgrading,
                       kDeviceStateActivating, kDeviceStateAudioTesting, kDeviceStateFatalError}) {
        idle.Activity(0);
        assert(!idle.Tick(timeout * 2, state, false));
        assert(!idle.Tick(timeout * 3 - 1, kDeviceStateIdle, false));
        assert(idle.Tick(timeout * 3, kDeviceStateIdle, false));
    }
    idle.Activity(0);
    assert(!idle.Tick(timeout - 1, kDeviceStateListening, true));
    assert(!idle.Tick(timeout * 2 - 2, kDeviceStateListening, false));
    assert(idle.Tick(timeout * 2 - 1, kDeviceStateListening, false));
    assert(!idle.Tick(timeout * 2, kDeviceStateListening, true));
    idle.Activity(9000000000000LL);
    assert(!idle.Tick(9000000000001LL, kDeviceStateIdle, false));

    std::cout << "Idle boundaries, wake consumption, voice activity, protected states, "
                 "and large timestamps passed.\n";
}
