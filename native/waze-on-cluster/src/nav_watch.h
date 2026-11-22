// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Route guidance schema, wire decoders and state machine for Show when:
 * CarPlayManager (projectedGuidanceActive), AndroidAutoProjectedManager
 * (projectedGuidanceActive), and Guidance (guidanceState, guidingProvider).
 *
 * Service, instance, version, field and enum ids default to the MGU iDrive 7
 * catalog and are updated at start from /etc/oap/someip/someip-interfaces.json
 * when present so firmware I-Steps with another major version or id match. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace woc {

struct NavFieldSpec {
    uint16_t getter = 0;
    bool getter_reliable = false;
    uint16_t notifier = 0;
    uint16_t eventgroup = 0;
    bool notifier_reliable = false;
};

struct NavServiceSpec {
    uint16_t service = 0;
    uint16_t instance = 0x0080;
    uint8_t major = 0;
};

struct NavSchema {
    NavServiceSpec carplay{0xFB61, 0x0080, 0};
    NavFieldSpec carplay_projected{0x0002, false, 0x8002, 2, false};

    NavServiceSpec aa{0xFB73, 0x0080, 1};
    NavFieldSpec aa_projected{0x0002, false, 0x8002, 2, false};

    NavServiceSpec guidance{0xFB1A, 0x0080, 7};
    NavFieldSpec guidance_state{0x0002, false, 0x8002, 2, false};
    NavFieldSpec guiding_provider{0x001B, false, 0x8015, 21, false};

    uint32_t state_active = 2;       /* GuidanceState::ACTIVE */
    uint32_t provider_carplay = 1;   /* ProjectedModeGuidingProvider::CAR_PLAY */
    uint32_t provider_aa = 2;        /* ProjectedModeGuidingProvider::ANDROID_AUTO */
};

struct NavState {
    bool phone = false;
    bool bmw = false;
    bool operator==(const NavState &o) const { return phone == o.phone && bmw == o.bmw; }
    bool operator!=(const NavState &o) const { return !(*this == o); }
};

/* Reads service/instance/version/field/enum definitions from a SOME/IP catalog
 * JSON file (e.g. /etc/oap/someip/someip-interfaces.json), keeping defaults
 * for anything absent or unreadable. */
NavSchema load_nav_schema(const std::string &path);

/* CommonAPI SOME/IP scalar decoders: Bool (1 byte 0/1) and big-endian enum
 * (1, 2 or 4 bytes). */
bool decode_nav_bool(const uint8_t *p, size_t n, bool *out);
bool decode_nav_uint(const uint8_t *p, size_t n, uint32_t *out);

class NavStateTracker {
public:
    explicit NavStateTracker(const NavSchema &schema = NavSchema()) : schema_(schema) {}

    void set_carplay_available(bool up);
    void set_carplay_projected(bool active);
    void set_aa_available(bool up);
    void set_aa_projected(bool active);
    void set_guidance_available(bool up);
    void set_guidance_state(uint32_t state);
    void set_guiding_provider(uint32_t provider);

    bool carplay_seen() const { return cp_seen_; }
    bool aa_seen() const { return aa_seen_; }
    bool guidance_state_seen() const { return gs_seen_; }
    bool guiding_provider_seen() const { return gp_seen_; }

    NavState state() const;

private:
    NavSchema schema_;
    bool cp_seen_ = false;
    bool cp_active_ = false;
    bool aa_seen_ = false;
    bool aa_active_ = false;
    bool gs_seen_ = false;
    bool gp_seen_ = false;
    uint32_t guidance_state_ = 0;
    uint32_t guiding_provider_ = 0;
};

}  // namespace woc
