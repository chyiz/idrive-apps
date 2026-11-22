// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "nav_watch.h"

#include <cstdio>
#include <fstream>
#include <unistd.h>

using namespace woc;

static int failures = 0;
static void ok(const char *name, bool cond)
{
    std::printf("%s  %s\n", cond ? "ok " : "FAIL", name);
    if (!cond)
        failures++;
}

int main()
{
    const NavSchema def;
    ok("defaults: MGU service, instance, version and field ids",
       def.carplay.service == 0xFB61 && def.carplay.instance == 0x0080 && def.carplay.major == 0 &&
           def.carplay_projected.getter == 2 && def.carplay_projected.notifier == 0x8002 &&
           def.carplay_projected.eventgroup == 2 && def.aa.service == 0xFB73 && def.aa.major == 1 &&
           def.guidance.service == 0xFB1A && def.guidance.major == 7 && def.guidance_state.getter == 2 &&
           def.guiding_provider.getter == 27 && def.guiding_provider.notifier == 0x8015 &&
           def.guiding_provider.eventgroup == 21);

    const std::string path = "/tmp/woc-nav-schema-" + std::to_string(getpid()) + ".json";
    unlink(path.c_str());
    ok("missing catalog file: defaults kept", load_nav_schema(path).guidance.major == 7);

    std::ofstream(path) << R"({
      "datatypes": {
        "de.bmw.infotainment.navigation.GuidingTypes.GuidanceState": {
          "items": { "ACTIVE": 5, "CALCULATING_ROUTE": 1, "INACTIVE": 0 },
          "name": "GuidanceState",
          "type": "enum"
        },
        "de.bmw.infotainment.navigation.GuidingTypes.ProjectedModeGuidingProvider": {
          "items": { "ANDROID_AUTO": 4, "CAR_PLAY": 3, "NONE": 0 },
          "name": "ProjectedModeGuidingProvider",
          "type": "enum"
        }
      },
      "instanceIDs": {
        "de.bmw.infotainment.a4a.carplay.CarPlayManager": { "CarPlayManagerInst1": 129 },
        "de.bmw.infotainment.navigation.guiding.Guidance": { "Main_Instance": 130 }
      },
      "interfaces": {
        "de.bmw.infotainment.a4a.carplay.CarPlayManager": {
          "serviceId": 64353,
          "version": { "major": 2, "minor": 1 },
          "fields": {
            "projectedGuidanceActive": {
              "dataRef": "Bool",
              "getter": { "id": 10, "reliable": true },
              "notifier": { "eventgroups": [7], "id": 32780, "reliable": true }
            }
          }
        },
        "de.bmw.infotainment.navigation.guiding.Guidance": {
          "serviceId": 64282,
          "version": { "major": 6, "minor": 0 },
          "fields": {
            "guidanceState": {
              "getter": { "id": 3 },
              "notifier": { "eventgroups": [4], "id": 32771 }
            },
            "guidingProvider": {
              "getter": { "id": 28 },
              "notifier": { "eventgroups": [22], "id": 32790 }
            }
          }
        }
      }
    })";
    const NavSchema loaded = load_nav_schema(path);
    unlink(path.c_str());
    ok("catalog overrides: version, instance, getter, notifier, reliability and enum values read; AA kept default",
       loaded.carplay.major == 2 && loaded.carplay.instance == 129 && loaded.carplay_projected.getter == 10 &&
           loaded.carplay_projected.getter_reliable && loaded.carplay_projected.notifier == 32780 &&
           loaded.carplay_projected.eventgroup == 7 && loaded.carplay_projected.notifier_reliable &&
           loaded.guidance.major == 6 && loaded.guidance.instance == 130 && loaded.guidance_state.getter == 3 &&
           loaded.guiding_provider.eventgroup == 22 && loaded.state_active == 5 &&
           loaded.provider_carplay == 3 && loaded.provider_aa == 4 && loaded.aa.service == 0xFB73);

    bool bv = true;
    const uint8_t b0[] = {0}, b1[] = {1}, b2[] = {2}, b2len[] = {1, 0};
    ok("decode_nav_bool: 1-byte 0/1 accepted, other values or lengths rejected",
       decode_nav_bool(b0, 1, &bv) && !bv && decode_nav_bool(b1, 1, &bv) && bv &&
           !decode_nav_bool(b2, 1, &bv) && !decode_nav_bool(b2len, 2, &bv) && !decode_nav_bool(nullptr, 1, &bv));

    uint32_t uv = 0;
    const uint8_t u1[] = {2}, u2[] = {0, 2}, u4[] = {0, 0, 0, 2}, u3[] = {0, 0, 2};
    ok("decode_nav_uint: 1, 2 and 4 byte big-endian enums decoded, 3-byte rejected",
       decode_nav_uint(u1, 1, &uv) && uv == 2 && decode_nav_uint(u2, 2, &uv) && uv == 2 &&
           decode_nav_uint(u4, 4, &uv) && uv == 2 && !decode_nav_uint(u3, 3, &uv));

    NavStateTracker tr;
    ok("initial state: both idle", tr.state() == NavState{false, false});
    tr.set_guiding_provider(1);
    ok("before CarPlayManager answers: guidingProvider CAR_PLAY counts as phone active",
       tr.state() == NavState{true, false});
    tr.set_carplay_projected(false);
    ok("once CarPlayManager reports false: lingering CAR_PLAY provider ignored",
       tr.state() == NavState{false, false});
    tr.set_carplay_projected(true);
    ok("CarPlay projectedGuidanceActive true: phone active", tr.state() == NavState{true, false});
    tr.set_guidance_state(2);
    ok("BMW Guidance ACTIVE takes immediate precedence over phone flag",
       tr.state() == NavState{false, true});
    tr.set_guidance_state(0);
    ok("BMW Guidance INACTIVE returns to active CarPlay route",
       tr.state() == NavState{true, false});
    tr.set_carplay_available(false);
    tr.set_guiding_provider(0);
    ok("CarPlay service unavailable clears CarPlay route", tr.state() == NavState{false, false});
    tr.set_guiding_provider(2);
    ok("Android Auto fallback via guidingProvider before AA manager reports",
       tr.state() == NavState{true, false});
    tr.set_aa_projected(false);
    ok("AndroidAutoProjectedManager false overrides lingering ANDROID_AUTO provider",
       tr.state() == NavState{false, false});
    tr.set_aa_projected(true);
    ok("AndroidAutoProjectedManager true sets phone active", tr.state() == NavState{true, false});

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
