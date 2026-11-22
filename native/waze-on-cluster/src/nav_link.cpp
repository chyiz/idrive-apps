// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "nav_link.h"

#include <chrono>
#include <fcntl.h>
#include <set>
#include <unistd.h>

#include <vsomeip/vsomeip.hpp>

#include "log.h"

namespace woc {

namespace {

vsomeip::reliability_type_e field_reliability(bool reliable)
{
    return reliable ? vsomeip::reliability_type_e::RT_RELIABLE : vsomeip::reliability_type_e::RT_UNKNOWN;
}

void register_field(const std::shared_ptr<vsomeip::application> &app, const NavServiceSpec &svc,
                    const NavFieldSpec &f, const char *label)
{
    app->request_event(svc.service, svc.instance, f.notifier, std::set<vsomeip::eventgroup_t>{f.eventgroup},
                       vsomeip::event_type_e::ET_FIELD, field_reliability(f.notifier_reliable));
    app->register_subscription_status_handler(
        svc.service, svc.instance, f.eventgroup, f.notifier,
        [svc, f, label](vsomeip::service_t, vsomeip::instance_t, vsomeip::eventgroup_t, vsomeip::event_t,
                        uint16_t code) {
            if (code != 0)
                log_warn("nav: subscription refused for %s (0x%04x eg %u ev 0x%04x): code %u", label,
                         (unsigned)svc.service, (unsigned)f.eventgroup, (unsigned)f.notifier, (unsigned)code);
        },
        true);
}

void subscribe_field(const std::shared_ptr<vsomeip::application> &app, const NavServiceSpec &svc,
                     const NavFieldSpec &f)
{
    app->subscribe(svc.service, svc.instance, f.eventgroup, svc.major, f.notifier);
}

void release_field(const std::shared_ptr<vsomeip::application> &app, const NavServiceSpec &svc,
                   const NavFieldSpec &f)
{
    app->unsubscribe(svc.service, svc.instance, f.eventgroup);
    app->unregister_subscription_status_handler(svc.service, svc.instance, f.eventgroup, f.notifier);
    app->release_event(svc.service, svc.instance, f.notifier);
}

}  // namespace

NavLink::NavLink()
{
    if (pipe(wake_) < 0) {
        wake_[0] = wake_[1] = -1;
        return;
    }
    for (int fd : wake_) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0)
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

NavLink::~NavLink()
{
    stop();
    if (wake_[0] >= 0)
        close(wake_[0]);
    if (wake_[1] >= 0)
        close(wake_[1]);
}

bool NavLink::start(const NavSchema &schema, const std::string &app_name)
{
    if (wake_[0] < 0) {
        log_warn("nav: wakeup pipe failed; route guidance tracking disabled");
        return false;
    }
    schema_ = schema;
    tracker_ = NavStateTracker(schema_);
    /* No VSOMEIP_CONFIGURATION: loads /etc/vsomeip routed by vsomeipd under
     * hmi_5000 (DCSM.md "Running as uid 5000"). */
    app_ = vsomeip::runtime::get()->create_application(app_name);
    if (!app_ || !app_->init()) {
        log_warn("nav: vsomeip application %s did not initialise; route guidance tracking disabled",
                 app_name.c_str());
        app_.reset();
        return false;
    }
    log_info("nav: vsomeip %s watching Guidance 0x%04x v%u, CarPlay 0x%04x v%u, AndroidAuto 0x%04x v%u",
             app_name.c_str(), (unsigned)schema_.guidance.service, (unsigned)schema_.guidance.major,
             (unsigned)schema_.carplay.service, (unsigned)schema_.carplay.major,
             (unsigned)schema_.aa.service, (unsigned)schema_.aa.major);

    const NavServiceSpec svcs[3] = {schema_.carplay, schema_.aa, schema_.guidance};
    for (const NavServiceSpec &svc : svcs) {
        app_->register_message_handler(svc.service, svc.instance, vsomeip::ANY_METHOD,
                                       [this](const std::shared_ptr<vsomeip::message> &m) { on_message(m); });
        app_->register_availability_handler(
            svc.service, svc.instance,
            [this](vsomeip::service_t s, vsomeip::instance_t, bool up) { on_available(s, up); }, svc.major,
            vsomeip::ANY_MINOR);
    }

    register_field(app_, schema_.carplay, schema_.carplay_projected, "CarPlay.projectedGuidanceActive");
    register_field(app_, schema_.aa, schema_.aa_projected, "AndroidAuto.projectedGuidanceActive");
    register_field(app_, schema_.guidance, schema_.guidance_state, "Guidance.guidanceState");
    register_field(app_, schema_.guidance, schema_.guiding_provider, "Guidance.guidingProvider");

    for (const NavServiceSpec &svc : svcs)
        app_->request_service(svc.service, svc.instance, svc.major, vsomeip::ANY_MINOR);

    subscribe_field(app_, schema_.carplay, schema_.carplay_projected);
    subscribe_field(app_, schema_.aa, schema_.aa_projected);
    subscribe_field(app_, schema_.guidance, schema_.guidance_state);
    subscribe_field(app_, schema_.guidance, schema_.guiding_provider);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
    }
    loop_ = std::thread([this] { app_->start(); });
    poller_ = std::thread([this] { poll_loop(); });
    return true;
}

void NavLink::stop()
{
    if (!app_)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
            return;
        running_ = false;
    }
    cv_.notify_all();
    if (poller_.joinable())
        poller_.join();

    release_field(app_, schema_.carplay, schema_.carplay_projected);
    release_field(app_, schema_.aa, schema_.aa_projected);
    release_field(app_, schema_.guidance, schema_.guidance_state);
    release_field(app_, schema_.guidance, schema_.guiding_provider);

    const NavServiceSpec svcs[3] = {schema_.carplay, schema_.aa, schema_.guidance};
    for (const NavServiceSpec &svc : svcs) {
        app_->release_service(svc.service, svc.instance);
        app_->unregister_availability_handler(svc.service, svc.instance, svc.major, vsomeip::ANY_MINOR);
        app_->unregister_message_handler(svc.service, svc.instance, vsomeip::ANY_METHOD);
    }

    app_->stop();
    if (loop_.joinable())
        loop_.join();
    app_.reset();
}

void NavLink::send_getter(const NavServiceSpec &svc, const NavFieldSpec &field)
{
    if (!field.getter)
        return;
    auto rt = vsomeip::runtime::get();
    std::shared_ptr<vsomeip::message> m = rt->create_request(field.getter_reliable);
    m->set_service(svc.service);
    m->set_instance(svc.instance);
    m->set_method(field.getter);
    m->set_interface_version(svc.major);
    app_->send(m);
}

void NavLink::poll_loop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        const bool need_initial = (cp_up_ && !tracker_.carplay_seen()) || (aa_up_ && !tracker_.aa_seen()) ||
                                  (guidance_up_ && (!tracker_.guidance_state_seen() || !tracker_.guiding_provider_seen()));
        cv_.wait_for(lock, std::chrono::milliseconds(need_initial ? 2000 : 15000));
        if (!running_)
            break;
        const bool ask_cp = cp_up_;
        const bool ask_aa = aa_up_;
        const bool ask_g = guidance_up_;
        lock.unlock();
        if (ask_cp)
            send_getter(schema_.carplay, schema_.carplay_projected);
        if (ask_aa)
            send_getter(schema_.aa, schema_.aa_projected);
        if (ask_g) {
            send_getter(schema_.guidance, schema_.guidance_state);
            send_getter(schema_.guidance, schema_.guiding_provider);
        }
        lock.lock();
    }
}

void NavLink::on_available(uint16_t service, bool up)
{
    bool ask_cp = false, ask_aa = false, ask_g = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (service == schema_.carplay.service) {
            if (cp_up_ == up)
                return;
            cp_up_ = up;
            tracker_.set_carplay_available(up);
            ask_cp = up;
        } else if (service == schema_.aa.service) {
            if (aa_up_ == up)
                return;
            aa_up_ = up;
            tracker_.set_aa_available(up);
            ask_aa = up;
        } else if (service == schema_.guidance.service) {
            if (guidance_up_ == up)
                return;
            guidance_up_ = up;
            tracker_.set_guidance_available(up);
            ask_g = up;
        }
        publish_locked();
    }
    if (ask_cp)
        send_getter(schema_.carplay, schema_.carplay_projected);
    if (ask_aa)
        send_getter(schema_.aa, schema_.aa_projected);
    if (ask_g) {
        send_getter(schema_.guidance, schema_.guidance_state);
        send_getter(schema_.guidance, schema_.guiding_provider);
    }
    cv_.notify_all();
}

void NavLink::on_message(const std::shared_ptr<vsomeip::message> &m)
{
    const auto mt = m->get_message_type();
    if (mt != vsomeip::message_type_e::MT_NOTIFICATION &&
        (mt != vsomeip::message_type_e::MT_RESPONSE || m->get_return_code() != vsomeip::return_code_e::E_OK)) {
        return;
    }
    std::shared_ptr<vsomeip::payload> body = m->get_payload();
    const uint8_t *p = body ? body->get_data() : nullptr;
    const size_t n = body ? body->get_length() : 0;
    const uint16_t svc = m->get_service();
    const uint16_t method = m->get_method();

    std::lock_guard<std::mutex> lock(mutex_);
    if (svc == schema_.carplay.service &&
        (method == schema_.carplay_projected.notifier || method == schema_.carplay_projected.getter)) {
        bool v = false;
        if (decode_nav_bool(p, n, &v)) {
            tracker_.set_carplay_projected(v);
            publish_locked();
        } else if (!bad_cp_logged_) {
            bad_cp_logged_ = true;
            log_warn("nav: undecoded CarPlay projectedGuidanceActive payload (%zu bytes)", n);
        }
    } else if (svc == schema_.aa.service &&
               (method == schema_.aa_projected.notifier || method == schema_.aa_projected.getter)) {
        bool v = false;
        if (decode_nav_bool(p, n, &v)) {
            tracker_.set_aa_projected(v);
            publish_locked();
        } else if (!bad_aa_logged_) {
            bad_aa_logged_ = true;
            log_warn("nav: undecoded AndroidAuto projectedGuidanceActive payload (%zu bytes)", n);
        }
    } else if (svc == schema_.guidance.service &&
               (method == schema_.guidance_state.notifier || method == schema_.guidance_state.getter)) {
        uint32_t v = 0;
        if (decode_nav_uint(p, n, &v)) {
            tracker_.set_guidance_state(v);
            publish_locked();
        } else if (!bad_gs_logged_) {
            bad_gs_logged_ = true;
            log_warn("nav: undecoded Guidance guidanceState payload (%zu bytes)", n);
        }
    } else if (svc == schema_.guidance.service &&
               (method == schema_.guiding_provider.notifier || method == schema_.guiding_provider.getter)) {
        uint32_t v = 0;
        if (decode_nav_uint(p, n, &v)) {
            tracker_.set_guiding_provider(v);
            publish_locked();
        } else if (!bad_gp_logged_) {
            bad_gp_logged_ = true;
            log_warn("nav: undecoded Guidance guidingProvider payload (%zu bytes)", n);
        }
    }
}

void NavLink::publish_locked()
{
    const NavState next = tracker_.state();
    if (next == pushed_ && !pending_)
        return;
    pushed_ = next;
    pending_ = true;
    char c = 0;
    ssize_t w = write(wake_[1], &c, 1);
    (void)w;
}

bool NavLink::drain(NavState *out)
{
    if (wake_[0] < 0)
        return false;
    char buf[64];
    while (read(wake_[0], buf, sizeof buf) > 0) {
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_)
        return false;
    pending_ = false;
    if (pushed_ == reported_)
        return false;
    reported_ = pushed_;
    *out = reported_;
    return true;
}

}  // namespace woc
