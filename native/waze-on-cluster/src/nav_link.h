// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Watches CarPlay, Android Auto and BMW route guidance on the head unit's
 * vsomeip bus (running as uid 5000, hmi_5000) and wakes the main poll loop
 * through a pipe whenever the active guidance state changes. */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "nav_watch.h"

namespace vsomeip_v3 {
class application;
class message;
}  // namespace vsomeip_v3

namespace woc {

class NavLink {
public:
    NavLink();
    ~NavLink();

    NavLink(const NavLink &) = delete;
    NavLink &operator=(const NavLink &) = delete;

    bool start(const NavSchema &schema, const std::string &app_name);
    void stop();

    int fd() const { return wake_[0]; }
    /* Drains the wakeup pipe; returns true and writes *out if state changed. */
    bool drain(NavState *out);

private:
    void on_message(const std::shared_ptr<vsomeip_v3::message> &m);
    void on_available(uint16_t service, bool up);
    void send_getter(const NavServiceSpec &svc, const NavFieldSpec &field);
    void poll_loop();
    void publish_locked();

    int wake_[2];
    NavSchema schema_;
    std::shared_ptr<vsomeip_v3::application> app_;
    std::thread loop_;
    std::thread poller_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool cp_up_ = false;
    bool aa_up_ = false;
    bool guidance_up_ = false;
    bool bad_cp_logged_ = false;
    bool bad_aa_logged_ = false;
    bool bad_gs_logged_ = false;
    bool bad_gp_logged_ = false;
    NavStateTracker tracker_;
    NavState pushed_;
    NavState reported_;
    bool pending_ = false;
};

}  // namespace woc
