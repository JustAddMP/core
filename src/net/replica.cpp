#include "net/replica.h"

#include <algorithm>
#include <cmath>

namespace mc::net::replica {
namespace {

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void FromSample(const Sample& s, Out& o) {
    o.pos = s.pos;
    o.vel = s.vel;
    for (int i = 0; i < 4; ++i) o.rot[i] = s.rot[i];
    o.hasRot = s.hasRot;
    o.bits = s.bits;
    o.fromT = o.toT = s.t;
    o.u = 0;
    o.valid = true;
}

}  // namespace

// ---- SnapshotTrack ---------------------------------------------------------------------------------------------
SnapshotTrack::SnapshotTrack(const TrackTuning& t) : t_(t) {
    if (t_.capacity < 2) t_.capacity = 2;
    ring_.resize(t_.capacity);
}

void SnapshotTrack::Clear() { head_ = size_ = 0; }

const Sample& SnapshotTrack::Get(size_t i) const { return ring_[(head_ + i) % ring_.size()]; }
const Sample& SnapshotTrack::Newest() const { return Get(size_ - 1); }
const Sample& SnapshotTrack::Oldest() const { return Get(0); }

bool SnapshotTrack::Push(const Sample& s) {
    if (!(s.t == s.t)) return false;  // NaN
    if (size_ > 0 && s.t <= Newest().t) return false;
    if (size_ == ring_.size()) {
        head_ = (head_ + 1) % ring_.size();
        --size_;
    }
    ring_[(head_ + size_) % ring_.size()] = s;
    ++size_;
    return true;
}

Out SnapshotTrack::At(double rt) const {
    Out o;
    if (size_ == 0) return o;
    const Sample& first = Oldest();
    const Sample& last = Newest();
    if (rt <= first.t && size_ > 1) {
        FromSample(first, o);
        return o;
    }
    if (rt >= last.t || size_ == 1) {
        FromSample(last, o);
        const double past = std::max(0.0, rt - last.t);
        o.starved = past;
        o.extrapolated = past > 0;
        o.held = past > t_.maxExtrapolate;
        Vec3 acc;
        if (t_.accelTime > 0 && size_ > 1) {
            const Sample& prev = Get(size_ - 2);
            const double d = last.t - prev.t;
            if (d > 1e-4 && Dist(prev.pos, last.pos) <= t_.teleportDist) {
                acc = {static_cast<float>((last.vel.x - prev.vel.x) / d), static_cast<float>((last.vel.y - prev.vel.y) / d),
                       static_cast<float>((last.vel.z - prev.vel.z) / d)};
            }
        }
        o.pos = playout::Extrapolate(last.pos, last.vel, acc, past, t_.maxExtrapolate, t_.accelTime);
        return o;
    }
    // first.t < rt < last.t: the pair around rt (binary search over the ring)
    size_t lo = 0, hi = size_ - 1;  // Get(lo).t < rt <= Get(hi).t
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (Get(mid).t < rt)
            lo = mid;
        else
            hi = mid;
    }
    const Sample& a = Get(lo);
    const Sample& b = Get(hi);
    const double span = std::max(1e-6, b.t - a.t);
    const float u = static_cast<float>((rt - a.t) / span);
    const Sample& nearer = u < 0.5f ? a : b;
    if (Dist(a.pos, b.pos) > t_.teleportDist) {
        FromSample(nearer, o);
        o.teleport = true;
        return o;
    }
    o.valid = true;
    o.bits = nearer.bits;
    o.fromT = a.t;
    o.toT = b.t;
    o.u = u;
    o.pos = playout::Hermite(a.pos, a.vel, b.pos, b.vel, b.t - a.t, u);
    auto lerp = [u](float x, float y) { return x + (y - x) * u; };
    o.vel = {lerp(a.vel.x, b.vel.x), lerp(a.vel.y, b.vel.y), lerp(a.vel.z, b.vel.z)};
    if (a.hasRot && b.hasRot) {
        motion::Nlerp(a.rot, b.rot, u, o.rot);
        o.hasRot = true;
    } else {
        for (int i = 0; i < 4; ++i) o.rot[i] = nearer.rot[i];
        o.hasRot = nearer.hasRot;
    }
    return o;
}

Out SnapshotTrack::Present(double rt, double lead) const {
    Out o = At(rt);
    if (!o.valid) return o;
    const float l = static_cast<float>(std::max(0.0, lead));
    const Vec3 d = {o.vel.x * l, o.vel.y * l, o.vel.z * l};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float k = len > t_.maxLead ? t_.maxLead / len : 1.0f;
    o.pos.x += d.x * k;
    o.pos.y += d.y * k;
    o.pos.z += d.z * k;
    return o;
}

// ---- Stream ----------------------------------------------------------------------------------------------------
Stream::Stream(const StreamTuning& t) : t_(t), clock_(t.playout) {}

void Stream::Reset() {
    clock_.Reset();
    tracks_.clear();
    lastLocal_ = 0;
    dropped_ = 0;
}

void Stream::Observe(double senderTime, double localTime) {
    clock_.Observe(senderTime, localTime);
    lastLocal_ = localTime;
}

bool Stream::Push(uint32_t key, const Sample& s) {
    auto it = tracks_.find(key);
    if (it == tracks_.end()) it = tracks_.emplace(key, Entry{SnapshotTrack(t_.track), lastLocal_}).first;
    if (!it->second.track.Push(s)) {
        ++dropped_;
        return false;
    }
    it->second.lastLocal = lastLocal_;
    return true;
}

void Stream::Update(double localNow) { clock_.Update(localNow); }

Out Stream::At(uint32_t key, double localNow) const {
    const SnapshotTrack* tr = Track(key);
    return tr ? tr->At(RenderTime(localNow)) : Out{};
}

Out Stream::Present(uint32_t key, double localNow) const {
    const SnapshotTrack* tr = Track(key);
    return tr ? tr->Present(RenderTime(localNow), Delay()) : Out{};
}

const SnapshotTrack* Stream::Track(uint32_t key) const {
    auto it = tracks_.find(key);
    return it == tracks_.end() ? nullptr : &it->second.track;
}

SnapshotTrack* Stream::Track(uint32_t key) {
    auto it = tracks_.find(key);
    return it == tracks_.end() ? nullptr : &it->second.track;
}

bool Stream::Remove(uint32_t key) { return tracks_.erase(key) > 0; }

std::vector<uint32_t> Stream::Forget(double olderThanLocal) {
    std::vector<uint32_t> gone;
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (it->second.lastLocal < olderThanLocal) {
            gone.push_back(it->first);
            it = tracks_.erase(it);
        } else {
            ++it;
        }
    }
    return gone;
}

std::vector<uint32_t> Stream::Keys() const {
    std::vector<uint32_t> k;
    k.reserve(tracks_.size());
    for (const auto& [key, e] : tracks_) k.push_back(key);
    return k;
}

double Stream::LastArrival(uint32_t key) const {
    auto it = tracks_.find(key);
    return it == tracks_.end() ? -1 : it->second.lastLocal;
}

}  // namespace mc::net::replica
