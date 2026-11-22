// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "nav_watch.h"

#include <cstdlib>
#include <fstream>

#include "log.h"

namespace woc {

namespace {

const size_t kMaxSchemaBytes = 16 * 1024 * 1024;

void skip_ws(const std::string &s, size_t *i)
{
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n'))
        (*i)++;
}

/* Advances *i past a JSON string literal starting at '"'. */
bool skip_json_string(const std::string &s, size_t *i)
{
    if (*i >= s.size() || s[*i] != '"')
        return false;
    (*i)++;
    while (*i < s.size()) {
        char c = s[(*i)++];
        if (c == '"')
            return true;
        if (c == '\\') {
            if (*i >= s.size())
                return false;
            (*i)++;
        }
    }
    return false;
}

/* Given s[*start] == open ('{' or '['), finds the matching close ('}' or ']'),
 * skipping braces inside strings. */
bool find_balanced(const std::string &s, size_t start, char open, char close, size_t *end)
{
    if (start >= s.size() || s[start] != open)
        return false;
    int depth = 0;
    size_t i = start;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            if (!skip_json_string(s, &i))
                return false;
            continue;
        }
        if (c == open) {
            depth++;
        } else if (c == close) {
            depth--;
            if (depth == 0) {
                *end = i + 1;
                return true;
            }
        }
        i++;
    }
    return false;
}

/* Advances *i past any JSON value at top-of-value position. */
bool skip_json_value(const std::string &s, size_t *i)
{
    skip_ws(s, i);
    if (*i >= s.size())
        return false;
    char c = s[*i];
    if (c == '"')
        return skip_json_string(s, i);
    if (c == '{') {
        size_t end = 0;
        if (!find_balanced(s, *i, '{', '}', &end))
            return false;
        *i = end;
        return true;
    }
    if (c == '[') {
        size_t end = 0;
        if (!find_balanced(s, *i, '[', ']', &end))
            return false;
        *i = end;
        return true;
    }
    size_t start = *i;
    while (*i < s.size() && s[*i] != ',' && s[*i] != '}' && s[*i] != ']' &&
           s[*i] != ' ' && s[*i] != '\t' && s[*i] != '\r' && s[*i] != '\n')
        (*i)++;
    return *i > start;
}

/* Finds direct member `key` inside JSON object `obj` (which starts with '{')
 * and writes its raw JSON slice into *val. */
bool json_member(const std::string &obj, const char *key, std::string *val)
{
    size_t i = 0;
    skip_ws(obj, &i);
    if (i >= obj.size() || obj[i] != '{')
        return false;
    i++;
    for (;;) {
        skip_ws(obj, &i);
        if (i >= obj.size() || obj[i] == '}')
            return false;
        if (obj[i] != '"')
            return false;
        size_t kstart = i + 1;
        if (!skip_json_string(obj, &i))
            return false;
        std::string k = obj.substr(kstart, i - 1 - kstart);
        skip_ws(obj, &i);
        if (i >= obj.size() || obj[i] != ':')
            return false;
        i++;
        skip_ws(obj, &i);
        size_t vstart = i;
        if (!skip_json_value(obj, &i))
            return false;
        if (k == key) {
            *val = obj.substr(vstart, i - vstart);
            return true;
        }
        skip_ws(obj, &i);
        if (i < obj.size() && obj[i] == ',') {
            i++;
            continue;
        }
        return false;
    }
}

bool json_uint(const std::string &raw, uint32_t *out)
{
    if (raw.empty())
        return false;
    char *end = nullptr;
    unsigned long v = std::strtoul(raw.c_str(), &end, 10);
    if (!end || *end != '\0')
        return false;
    *out = (uint32_t)v;
    return true;
}

bool json_first_array_uint(const std::string &arr, uint32_t *out)
{
    size_t i = 0;
    skip_ws(arr, &i);
    if (i >= arr.size() || arr[i] != '[')
        return false;
    i++;
    skip_ws(arr, &i);
    size_t start = i;
    while (i < arr.size() && arr[i] >= '0' && arr[i] <= '9')
        i++;
    if (i == start)
        return false;
    return json_uint(arr.substr(start, i - start), out);
}

void parse_field(const std::string &fields_obj, const char *name, NavFieldSpec *f)
{
    std::string fobj, sub, v;
    if (!json_member(fields_obj, name, &fobj))
        return;
    uint32_t n = 0;
    if (json_member(fobj, "getter", &sub)) {
        if (json_member(sub, "id", &v) && json_uint(v, &n) && n <= 0xFFFF)
            f->getter = (uint16_t)n;
        if (json_member(sub, "reliable", &v))
            f->getter_reliable = (v == "true");
    }
    if (json_member(fobj, "notifier", &sub)) {
        if (json_member(sub, "id", &v) && json_uint(v, &n) && n <= 0xFFFF)
            f->notifier = (uint16_t)n;
        if (json_member(sub, "eventgroups", &v) && json_first_array_uint(v, &n) && n <= 0xFFFF)
            f->eventgroup = (uint16_t)n;
        if (json_member(sub, "reliable", &v))
            f->notifier_reliable = (v == "true");
    }
}

void parse_service(const std::string &ifaces, const std::string &instances, const char *iface_name,
                   const char *inst_name, NavServiceSpec *svc, std::string *fields_out)
{
    std::string sobj, sub, v;
    uint32_t n = 0;
    if (json_member(ifaces, iface_name, &sobj)) {
        if (json_member(sobj, "serviceId", &v) && json_uint(v, &n) && n <= 0xFFFF)
            svc->service = (uint16_t)n;
        if (json_member(sobj, "version", &sub) && json_member(sub, "major", &v) && json_uint(v, &n) && n <= 0xFF)
            svc->major = (uint8_t)n;
        if (fields_out)
            json_member(sobj, "fields", fields_out);
    }
    if (!instances.empty() && json_member(instances, iface_name, &sub) && json_member(sub, inst_name, &v) &&
        json_uint(v, &n) && n <= 0xFFFF) {
        svc->instance = (uint16_t)n;
    }
}

void parse_enum_item(const std::string &dtypes, const char *type_name, const char *item_name, uint32_t *out)
{
    std::string tobj, items, v;
    uint32_t n = 0;
    if (json_member(dtypes, type_name, &tobj) && json_member(tobj, "items", &items) &&
        json_member(items, item_name, &v) && json_uint(v, &n)) {
        *out = n;
    }
}

}  // namespace

NavSchema load_nav_schema(const std::string &path)
{
    NavSchema s;
    if (path.empty())
        return s;
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in)
        return s;
    in.seekg(0, std::ios::end);
    std::streamoff len = in.tellg();
    if (len <= 0 || (size_t)len > kMaxSchemaBytes) {
        log_warn("nav: %s skipped (size %lld)", path.c_str(), (long long)len);
        return s;
    }
    std::string text((size_t)len, '\0');
    in.seekg(0, std::ios::beg);
    if (!in.read(&text[0], len))
        return s;

    std::string ifaces, instances, dtypes;
    if (!json_member(text, "interfaces", &ifaces)) {
        log_warn("nav: %s has no interfaces object; using built-in SOME/IP ids", path.c_str());
        return s;
    }
    json_member(text, "instanceIDs", &instances);
    json_member(text, "datatypes", &dtypes);

    std::string fields;
    parse_service(ifaces, instances, "de.bmw.infotainment.a4a.carplay.CarPlayManager", "CarPlayManagerInst1",
                  &s.carplay, &fields);
    if (!fields.empty())
        parse_field(fields, "projectedGuidanceActive", &s.carplay_projected);

    fields.clear();
    parse_service(ifaces, instances,
                  "de.bmw.infotainment.sdi.aap.androidautoprojectedmanager.AndroidAutoProjectedManager",
                  "AndroidAutoProjectedManagerInst1", &s.aa, &fields);
    if (!fields.empty())
        parse_field(fields, "projectedGuidanceActive", &s.aa_projected);

    fields.clear();
    parse_service(ifaces, instances, "de.bmw.infotainment.navigation.guiding.Guidance", "Main_Instance",
                  &s.guidance, &fields);
    if (!fields.empty()) {
        parse_field(fields, "guidanceState", &s.guidance_state);
        parse_field(fields, "guidingProvider", &s.guiding_provider);
    }

    if (!dtypes.empty()) {
        parse_enum_item(dtypes, "de.bmw.infotainment.navigation.GuidingTypes.GuidanceState", "ACTIVE",
                        &s.state_active);
        parse_enum_item(dtypes, "de.bmw.infotainment.navigation.GuidingTypes.ProjectedModeGuidingProvider",
                        "CAR_PLAY", &s.provider_carplay);
        parse_enum_item(dtypes, "de.bmw.infotainment.navigation.GuidingTypes.ProjectedModeGuidingProvider",
                        "ANDROID_AUTO", &s.provider_aa);
    }
    return s;
}

bool decode_nav_bool(const uint8_t *p, size_t n, bool *out)
{
    if (!p || n != 1 || (p[0] != 0 && p[0] != 1))
        return false;
    *out = (p[0] == 1);
    return true;
}

bool decode_nav_uint(const uint8_t *p, size_t n, uint32_t *out)
{
    if (!p || (n != 1 && n != 2 && n != 4))
        return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++)
        v = (v << 8) | p[i];
    *out = v;
    return true;
}

void NavStateTracker::set_carplay_available(bool up)
{
    if (!up) {
        cp_seen_ = false;
        cp_active_ = false;
    }
}

void NavStateTracker::set_carplay_projected(bool active)
{
    cp_seen_ = true;
    cp_active_ = active;
}

void NavStateTracker::set_aa_available(bool up)
{
    if (!up) {
        aa_seen_ = false;
        aa_active_ = false;
    }
}

void NavStateTracker::set_aa_projected(bool active)
{
    aa_seen_ = true;
    aa_active_ = active;
}

void NavStateTracker::set_guidance_available(bool up)
{
    if (!up) {
        gs_seen_ = false;
        gp_seen_ = false;
        guidance_state_ = 0;
        guiding_provider_ = 0;
    }
}

void NavStateTracker::set_guidance_state(uint32_t state)
{
    gs_seen_ = true;
    guidance_state_ = state;
}

void NavStateTracker::set_guiding_provider(uint32_t provider)
{
    gp_seen_ = true;
    guiding_provider_ = provider;
}

NavState NavStateTracker::state() const
{
    if (gs_seen_ && guidance_state_ == schema_.state_active)
        return NavState{false, true};
    const bool cp = cp_seen_ ? cp_active_ : (gp_seen_ && guiding_provider_ == schema_.provider_carplay);
    const bool aa = aa_seen_ ? aa_active_ : (gp_seen_ && guiding_provider_ == schema_.provider_aa);
    return NavState{cp || aa, false};
}

}  // namespace woc
