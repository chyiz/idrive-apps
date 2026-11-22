// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "map_feeds.h"

#include <algorithm>

#include "log.h"

namespace woc {

MapFeeds::MapFeeds(const FeedConfig &base, bool cluster, bool hud, unsigned hud_fps, unsigned map_mode)
    : base_(base), hud_fps_(hud_fps), map_mode_(map_mode <= 2 ? map_mode : 0), cluster_on_(cluster), hud_on_(hud)
{
    base_.target = "map";
    if (base_.hud_width_pct > kHudMaxPct)
        base_.hud_width_pct = kHudMaxPct;
    follow();
}

bool MapFeeds::allowed() const
{
    if (map_mode_ == 1)
        return phone_nav_;
    if (map_mode_ == 2)
        return !bmw_nav_;
    return true;
}

void MapFeeds::follow()
{
    const bool want_cluster = cluster_on_ && allowed();
    const bool want_hud = hud_on_ && allowed();
    /* A feed made while the other is there starts from its newest picture:
     * the HUD's is made first, so it can still take the cluster's. */
    if (want_hud && !hud_) {
        FeedConfig fc = base_;
        fc.target = "hud";
        fc.rect = MapRect();   /* map_rect is the cluster's */
        fc.video.fps = hud_fps_;
        /* the HUD's frames go exact, into debt, and wait while the HUD's
         * share is in debt: no packing on the renderer's thread for the HUD */
        fc.video.pan_blocks = false;
        fc.vpp_scale = false;   /* the HUD scales whole frames */
        fc.always_packed = false;   /* and goes exact (cluster map only) */
        hud_.reset(new MapFeed(fc));
        hud_->set_stream(live_, frozen_, picture_);
        hud_->changed(id_);
        log_info("hud: on, at most %u frames a second", hud_fps_);
    }
    if (want_cluster != !!cluster_) {
        /* off, its connection closes: the renderer drops its layer and the
         * KOMBI shows its own content again */
        if (want_cluster) {
            cluster_.reset(new MapFeed(base_));
            cluster_->set_stream(live_, frozen_, picture_);
            if (hud_ && hud_->latest(&copy_))
                cluster_->hold(&copy_, id_);
        } else {
            if (hud_ && hud_->picture_elsewhere() && cluster_->latest(&copy_))
                hud_->hold(&copy_, id_);
            cluster_.reset();
        }
        log_info("cluster: %s", want_cluster ? "on" : "off");
    }
    if (!want_hud && hud_) {
        /* its connection closes: the renderer drops its layer and lets the
         * HUD's slot go */
        hud_.reset();
        copy_ = std::vector<uint8_t>();
        log_info("hud: off");
    }
}

void MapFeeds::set_cluster(bool on)
{
    cluster_on_ = on;
    follow();
}

void MapFeeds::set_hud(bool on)
{
    hud_on_ = on;
    follow();
}

void MapFeeds::set_mode(unsigned mode)
{
    if (mode > 2 || mode == map_mode_)
        return;
    map_mode_ = mode;
    log_info("mode: %s", mode == 1 ? "only when navigating on phone"
                       : mode == 2 ? "yield to BMW navigation"
                                   : "always");
    follow();
}

void MapFeeds::set_nav(bool phone_nav, bool bmw_nav)
{
    if (phone_nav == phone_nav_ && bmw_nav == bmw_nav_)
        return;
    phone_nav_ = phone_nav;
    bmw_nav_ = bmw_nav;
    log_info("nav: phone %s, BMW %s", phone_nav ? "active" : "idle", bmw_nav ? "active" : "idle");
    follow();
}

bool MapFeeds::wants_frames() const
{
    return cluster_ || (hud_ && hud_->takes_frames());
}

void MapFeeds::set_hud_width(unsigned pct)
{
    if (pct > kHudMaxPct)
        pct = kHudMaxPct;
    base_.hud_width_pct = pct;
    if (hud_)
        hud_->set_hud_width(pct);
}

void MapFeeds::set_hud_offset(int dx, int dy)
{
    base_.hud_offset_x = dx;
    base_.hud_offset_y = dy;
    if (!hud_)
        return;
    hud_->set_hud_offset(dx, dy);
}

void MapFeeds::set_hud_keep_shown(bool keep)
{
    base_.hud_keep_shown = keep;
    if (hud_)
        hud_->set_hud_keep_shown(keep);
}

void MapFeeds::set_guide_hold(bool hold)
{
    base_.guide_hold = hold;
    if (cluster_)
        cluster_->set_guide_hold(hold);
}

size_t MapFeeds::poll_fds(std::vector<pollfd> *fds) const
{
    const size_t first = fds->size();
    for (const MapFeed *f : {cluster_.get(), hud_.get()})
        if (f)
            fds->push_back(pollfd{f->fd(), (short)(f->fd() >= 0 ? (POLLIN | (f->wants_write() ? POLLOUT : 0)) : 0), 0});
    return first;
}

void MapFeeds::poll(int64_t now_ms)
{
    if (cluster_)
        cluster_->poll(now_ms);
    if (hud_)
        hud_->poll(now_ms);
    feed_hud(now_ms);
}

void MapFeeds::feed_hud(int64_t now_ms)
{
    if (cluster_ && hud_ && hud_->takes_frames() && hud_->wants_picture(now_ms) && cluster_->latest(&copy_))
        hud_->offer(&copy_, id_, now_ms);
}

int64_t MapFeeds::wake_in_ms(int64_t now_ms) const
{
    const int64_t c = cluster_ ? cluster_->wake_in_ms(now_ms) : -1, h = hud_ ? hud_->wake_in_ms(now_ms) : -1;
    return c < 0 ? h : h < 0 ? c : std::min(c, h);
}

/* The cluster's without a copy. The HUD's copy of a new picture is made
 * only when its scale is due (feed_hud), so a still map, and pictures
 * between two of its frames, cost the HUD nothing. The HUD alone takes the
 * frame itself. */
void MapFeeds::offer(std::vector<uint8_t> *atlas, uint64_t id, int64_t now_ms)
{
    id_ = id;
    if (!cluster_) {
        if (hud_ && hud_->takes_frames())
            hud_->offer(atlas, id, now_ms);
        return;
    }
    cluster_->offer(atlas, id, now_ms);
    if (hud_)
        hud_->changed(id);
    feed_hud(now_ms);
}

void MapFeeds::offer_part(std::vector<uint8_t> *atlas, uint64_t id, const OutputPart &part, int64_t now_ms)
{
    id_ = id;
    if (cluster_)
        cluster_->offer_part(atlas, id, part, now_ms);
}

void MapFeeds::repeat(uint64_t id, int64_t now_ms)
{
    id_ = id;
    if (cluster_)
        cluster_->repeat(id, now_ms);
    if (hud_)
        hud_->repeat(id, now_ms);
    feed_hud(now_ms);
}

void MapFeeds::set_stream(bool live, bool frozen, Picture picture)
{
    live_ = live;
    frozen_ = frozen;
    picture_ = picture;
    if (cluster_)
        cluster_->set_stream(live, frozen, picture);
    if (hud_)
        hud_->set_stream(live, frozen, picture);
}

FeedStatus MapFeeds::status(const std::string &decode, const std::string &stream) const
{
    const bool gated = !allowed();
    FeedStatus s;
    if (cluster_) {
        s = cluster_->status(decode, stream);
    } else {
        s.state = cluster_on_ && gated ? "idle" : "off";
        s.target = base_.target;
        s.page = decode;
        if (cluster_on_ && gated)
            s.reason = map_mode_ == 1 ? (live_ ? "Waiting for phone navigation" : "") : "Yielding to BMW navigation";
        s.guide_hold = base_.guide_hold;
    }
    s.cluster = cluster_on_;
    s.hud = hud_on_;
    s.map_mode = map_mode_;
    s.phone_nav = phone_nav_;
    s.bmw_nav = bmw_nav_;
    if (!hud_) {
        s.hud_state = hud_on_ && gated ? (map_mode_ == 1 ? (live_ ? "waiting_nav" : "idle") : "yielding_bmw") : "off";
    } else {
        const FeedStatus h = hud_->status(decode, stream);
        s.hud_state = h.disabled ? "disabled" : h.state;
        if (!cluster_) {
            s.upload_cap_kb = h.upload_cap_kb;
            s.upload_reason = h.upload_reason;
        }
    }
    s.hud_width_pct = base_.hud_width_pct;
    s.hud_keep_shown = base_.hud_keep_shown;
    s.hud_offset_x = hud_ ? hud_->hud_offset_x() : base_.hud_offset_x;
    s.hud_offset_y = hud_ ? hud_->hud_offset_y() : base_.hud_offset_y;
    return s;
}

void MapFeeds::request_report()
{
    if (cluster_)
        cluster_->request_report();
    if (hud_)
        hud_->request_report();
}

std::string MapFeeds::report() const
{
    return (cluster_ ? cluster_->report() : std::string()) + (hud_ ? "--- hud\n" + hud_->report() : std::string());
}

}  // namespace woc
