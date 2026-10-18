#include "net/playout.h"

#include <algorithm>
#include <cmath>

namespace mc::net::playout {

PlayoutClock::PlayoutClock(const PlayoutTuning& t) : t_(t) { Reset(); }

void PlayoutClock::Reset() {
    bucketMin_.fill(0);
    bucketId_.fill(INT64_MIN);
    raw_.fill(0);
    rawCount_ = rawNext_ = 0;
    samples_ = late_ = 0;
    offset_ = lateness_ = interval_ = 0;
    target_ = t_.startDelay;
    total_ = 0;
    lastSender_ = 0;
    lastUpdate_ = -1;
    haveTotal_ = false;
}

void PlayoutClock::Observe(double senderTime, double localTime) {
    const double raw = localTime - senderTime;
    // A snapshot older than what is shown already came too late to interpolate with.
    if (haveTotal_ && senderTime < localTime - total_) ++late_;

    // Offset: minimum of (arrival - stamp) over the last offsetWindow, kept in time buckets so it expires.
    const double bucketLen = std::max(0.01, t_.offsetWindow / kBuckets);
    const auto id = static_cast<int64_t>(std::floor(localTime / bucketLen));
    const int slot = static_cast<int>(((id % kBuckets) + kBuckets) % kBuckets);
    if (bucketId_[slot] != id) {
        bucketId_[slot] = id;
        bucketMin_[slot] = raw;
    } else {
        bucketMin_[slot] = std::min(bucketMin_[slot], raw);
    }
    bool first = true;
    for (int i = 0; i < kBuckets; ++i) {
        if (bucketId_[i] == INT64_MIN || bucketId_[i] <= id - kBuckets) continue;
        if (first || bucketMin_[i] < offset_) offset_ = bucketMin_[i];
        first = false;
    }

    // Snapshot interval (sender clock); gaps beyond maxInterval are pauses. Reordered snapshots do not count.
    if (samples_ > 0) {
        const double d = senderTime - lastSender_;
        if (d > 0 && d < t_.maxInterval) interval_ = interval_ <= 0 ? d : interval_ + (d - interval_) * 0.1;
    }
    if (samples_ == 0 || senderTime > lastSender_) lastSender_ = senderTime;

    raw_[rawNext_] = raw;
    rawNext_ = (rawNext_ + 1) % kRing;
    rawCount_ = std::min(rawCount_ + 1, kRing);
    ++samples_;
    Recompute();
    if (!haveTotal_) {
        total_ = offset_ + target_;
        haveTotal_ = true;
    }
}

void PlayoutClock::Recompute() {
    std::array<double, kRing> lat{};
    for (int i = 0; i < rawCount_; ++i) lat[i] = std::max(0.0, raw_[i] - offset_);
    const int k = std::clamp(static_cast<int>(std::ceil(t_.percentile * rawCount_)) - 1, 0, rawCount_ - 1);
    std::nth_element(lat.begin(), lat.begin() + k, lat.begin() + rawCount_);
    lateness_ = lat[k];
    if (static_cast<int>(samples_) < t_.minSamples) {
        target_ = t_.startDelay;
        return;
    }
    target_ = std::clamp(lateness_ + interval_ * t_.intervalShare + t_.margin, t_.minDelay, t_.maxDelay);
}

void PlayoutClock::Update(double localNow) {
    const double dt = lastUpdate_ < 0 ? 0 : std::clamp(localNow - lastUpdate_, 0.0, 0.25);
    lastUpdate_ = localNow;
    if (!haveTotal_) return;
    const double want = offset_ + target_;
    const double diff = want - total_;
    if (std::fabs(diff) > t_.snapDist) {
        total_ = want;
    } else if (diff > 0) {
        total_ += std::min(diff, t_.raiseRate * dt);
    } else if (diff < -t_.lowerSlack) {
        total_ -= std::min(-diff, t_.lowerRate * dt);
    }
}

double PlayoutClock::Starved(double newestSenderTime, double localNow) const {
    return std::max(0.0, RenderTime(localNow) - newestSenderTime);
}

Vec3 Hermite(const Vec3& p0, const Vec3& v0, const Vec3& p1, const Vec3& v1, double dt, double u) {
    u = std::clamp(u, 0.0, 1.0);
    const double u2 = u * u, u3 = u2 * u;
    const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    const double d = std::max(dt, 0.0);
    auto c = [&](float a, float va, float b, float vb) {
        return static_cast<float>(h00 * a + h10 * d * va + h01 * b + h11 * d * vb);
    };
    return {c(p0.x, v0.x, p1.x, v1.x), c(p0.y, v0.y, p1.y, v1.y), c(p0.z, v0.z, p1.z, v1.z)};
}

Vec3 Extrapolate(const Vec3& p, const Vec3& v, const Vec3& a, double t, double maxT, double accelT) {
    t = std::clamp(t, 0.0, std::max(0.0, maxT));
    const double ta = std::min(t, std::max(0.0, accelT));
    // acceleration for the first ta seconds, then the velocity reached by then
    auto c = [&](float x, float vx, float ax) {
        const double vEnd = vx + ax * ta;
        return static_cast<float>(x + vx * ta + 0.5 * ax * ta * ta + vEnd * (t - ta));
    };
    return {c(p.x, v.x, a.x), c(p.y, v.y, a.y), c(p.z, v.z, a.z)};
}

}  // namespace mc::net::playout
