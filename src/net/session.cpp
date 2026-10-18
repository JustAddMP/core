#include "net/session.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "net/auth.h"
#include "net/net_internal.h"

namespace mc::net {
using namespace detail;

namespace {

constexpr uint32_t kHelloMagic = 0x504F434D;  // "MCOP"
constexpr size_t kMaxNameLen = 32;
constexpr size_t kMaxChatLen = 256;
constexpr size_t kMaxReasonLen = 256;
constexpr double kRejectLinger = 1.0;
constexpr double kPingTableInterval = 2.0;
// Session clock (docs/protocol.md "Session clock"): after a join the guest pings this often a few times, so its clock
// is in sync within a few 100 ms instead of after several seconds.
constexpr int kPingBurst = 8;
constexpr double kPingBurstInterval = 0.1;
constexpr double kClockSampleMaxAge = 30.0;  // s: samples older than this no longer count (clock drift)
constexpr size_t kMaxAuthKeys = 256;  // host: addresses with recent password failures kept before stale ones are pruned

enum Msg : uint8_t {
    kHello = 0x01,
    kWelcome = 0x02,
    kReject = 0x03,
    kPeerJoined = 0x04,
    kPeerLeft = 0x05,
    kPing = 0x06,
    kPong = 0x07,
    kChat = 0x08,
    kPingTable = 0x09,
    kChallenge = 0x0A,
    kAuth = 0x0B,
    kKicked = 0x0C,
    kPeerDropped = 0x0D,
    kApp = 0x10,
};

enum RejectCode : uint8_t {
    kRejectVersion = 1,
    kRejectFull = 2,
    kRejectBadHello = 3,
    kRejectPasswordRequired = 4,
    kRejectWrongPassword = 5,
    kRejectTooManyAttempts = 6,
    kRejectBanned = 7,
};

enum KickCode : uint8_t {
    kKickOnly = 1,
    kKickBanned = 2,
};

constexpr uint8_t kAppFlagReliable = 0x01;
constexpr uint8_t kAppFlagStamped = 0x02;  // u32 send stamp (sender's SessionTimeMs) before the payload (protocol 9)

// Welcome trailer flags (docs/protocol.md "Reconnect")
constexpr uint8_t kWelcomeResumed = 0x01;   // the host held our slot: same id as before the loss
constexpr uint8_t kWelcomeHasToken = 0x02;  // a resume token follows
constexpr const char* kHoldExpiredReason = "lost the connection and did not come back";

std::string CleanName(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    if (s.size() > kMaxNameLen) {
        // Cut at a UTF-8 character boundary so a multi-byte character is never split into invalid UTF-8.
        size_t cut = kMaxNameLen;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s.empty() ? std::string("Player") : s;
}

// Free text from the other side or for the wire (reasons): control characters become spaces, at most maxLen bytes
// (cut at a UTF-8 character boundary).
std::string CleanText(std::string s, size_t maxLen) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    if (s.size() > maxLen) {
        size_t cut = maxLen;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
    }
    return s;
}

// A spawn profile name ("visuals/hero/game_main_tommy_casual") or a short alias ("paulie"): letters, digits, '_',
// '/', '.', '-' only (it ends up in a Lua command line), at most kMaxModelLen bytes; anything else gives "".
constexpr size_t kMaxModelLen = 96;
std::string CleanModel(std::string s) {
    if (s.size() > kMaxModelLen) return {};
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '/' || c == '.' || c == '-')) return {};
    return s;
}

uint32_t NowMs() { return static_cast<uint32_t>(Now() * 1000.0); }

}  // namespace

Session::Session(Transport& transport, SessionConfig cfg) : transport_(transport), cfg_(std::move(cfg)) {
    cfg_.maxPlayers = std::clamp(cfg_.maxPlayers, 2, kMaxPlayers);
    cfg_.playerName = CleanName(cfg_.playerName);
}

Session::~Session() { Leave(); }

// ---------------------------------------------------------------------------------------------------------------
// lifecycle

void Session::Reset() {
    players_.clear();
    pending_.clear();
    limits_.clear();
    rateStats_ = {};
    traffic_.clear();
    rateLogPending_ = false;
    localId_ = kNoPlayer;
    hostPeer_ = kInvalidPeer;
    welcomed_ = false;
    helloSentAt_ = 0;
    secrets_.clear();
    held_.clear();
    droppedOthers_.clear();
    resumeToken_ = {};
    hasResumeToken_ = false;
    attemptOpen_ = false;  // (reconnectAttempts_ stays readable after a failed reconnect)
    reconnectUntil_ = reconnectNextAt_ = 0;
    reconnectOldId_ = kNoPlayer;
    pingSent_ = {};
    pingBurst_ = 0;
    clockSamples_ = {};
    clockNext_ = 0;
    clockSynced_ = false;
    clockOffset_ = 0;
    clockErrMs_ = 0;
}

bool Session::Host(uint16_t port) {
    if (state_ == SessionState::Hosting || state_ == SessionState::Connected || state_ == SessionState::Connecting)
        Leave();
    Reset();
    lastError_.clear();
    lastReject_ = RejectReason::None;
    kicked_ = 0;
    bans_.clear();  // a new hosted session starts with an empty ban list
    if (!transport_.Host(port)) {
        lastError_ = "could not start hosting";
        state_ = SessionState::Disconnected;
        return false;
    }
    state_ = SessionState::Hosting;
    localId_ = kHostPlayer;
    players_.push_back({kHostPlayer, cfg_.playerName, CleanModel(cfg_.playerModel), kInvalidPeer, 0});
    Log(kInfo, "session: hosting (%s), protocol %u build %u", transport_.Name(), cfg_.protocolVersion, cfg_.modBuild);
    return true;
}

bool Session::Join(std::string_view address) {
    if (state_ != SessionState::Idle && state_ != SessionState::Disconnected) Leave();
    Reset();
    reconnectAttempts_ = 0;
    reconnectReason_.clear();
    lastError_.clear();
    lastReject_ = RejectReason::None;
    kicked_ = 0;
    lastJoinAddress_ = std::string(address);
    hostPeer_ = transport_.Connect(address);
    if (!hostPeer_) {
        lastError_ = "invalid address";
        state_ = SessionState::Disconnected;
        return false;
    }
    state_ = SessionState::Connecting;
    Log(kInfo, "session: joining %s over %s", std::string(address).c_str(), transport_.Name());
    return true;
}

bool Session::RetryWithPassword(std::string password) {
    if (lastJoinAddress_.empty() || state_ == SessionState::Hosting) return false;
    cfg_.password = std::move(password);
    const std::string address = lastJoinAddress_;
    return Join(address);
}

void Session::Leave() {
    if (state_ == SessionState::Idle) return;
    if (state_ == SessionState::Hosting) {
        for (auto& p : players_)
            if (p.id != kHostPlayer) transport_.Disconnect(p.peer);
        for (auto& kv : pending_) transport_.Disconnect(kv.first);
        bans_.clear();  // the ban list lives as long as the hosted session
    } else if (hostPeer_) {
        transport_.Disconnect(hostPeer_);
    }
    transport_.Close();
    Reset();
    state_ = SessionState::Idle;
}

void Session::Fail(const std::string& reason) {
    Log(kWarn, "session: %s", reason.c_str());
    lastError_ = reason;
    if (hostPeer_) transport_.Disconnect(hostPeer_);
    transport_.Close();
    Reset();
    state_ = SessionState::Disconnected;
    if (onDisconnected) onDisconnected(reason);
}

// ---------------------------------------------------------------------------------------------------------------
// pump

void Session::Poll() {
    if (state_ == SessionState::Idle || state_ == SessionState::Disconnected) return;
    transport_.Poll([this](const Event& e) { OnEvent(e); });
    if (state_ == SessionState::Idle || state_ == SessionState::Disconnected) return;
    double now = Now();
    if (state_ == SessionState::Reconnecting) {
        GuestReconnectPoll(now);
        return;
    }

    if (state_ == SessionState::Hosting) {
        if (rateLogPending_) LogRateLimits(now);
        HostExpireHolds(now);
        std::vector<PeerId> drop, rejectNow;
        for (auto& [peer, pend] : pending_) {
            if (pend.kickAt > 0) {
                if (now >= pend.kickAt) drop.push_back(peer);
            } else if (pend.rejectAt > 0) {
                if (now >= pend.rejectAt) rejectNow.push_back(peer);
            } else if (now - pend.since > cfg_.helloTimeoutS) {
                drop.push_back(peer);
            }
        }
        for (PeerId p : rejectNow) {  // delayed answer to a wrong password
            Pending& pend = pending_[p];
            pend.rejectAt = 0;
            Reject(p, pend.rejectCode, pend.rejectReason);
        }
        for (PeerId p : drop) {
            pending_.erase(p);
            transport_.Disconnect(p);
        }
    }
    // The transport reports its own connect timeout; this only covers a host that connects but never answers.
    if (state_ == SessionState::Connecting && helloSentAt_ > 0 && now - helloSentAt_ > cfg_.welcomeTimeoutS) {
        Fail("no answer from host");
        return;
    }

    bool sent = false;
    if (now - lastPing_ >= (pingBurst_ > 0 ? kPingBurstInterval : cfg_.pingIntervalS)) {
        lastPing_ = now;
        SendPing(now);
        sent = true;
    }
    if (state_ == SessionState::Hosting && now - lastPingTable_ >= kPingTableInterval) {
        sent = true;
        lastPingTable_ = now;
        // Refresh from transport stats when available (smoother than our own ping samples).
        for (auto& p : players_) {
            PeerStats st;
            if (p.id != kHostPlayer && transport_.GetStats(p.peer, st) && st.rttMs > 0) p.rttMs = st.rttMs;
        }
        ByteWriter w(32);
        w.U8(kPingTable);
        w.U8(static_cast<uint8_t>(players_.size()));
        for (auto& p : players_) {
            w.U8(p.id);
            w.U16(static_cast<uint16_t>(std::min(p.rttMs, 65535.0f)));
        }
        BroadcastRaw(w, Reliability::Unreliable, kNoPlayer);
    }
    // A ping waiting for the next Poll would measure our frame time as network delay (and skew the clock sample).
    if (sent) transport_.Flush();
}

void Session::Flush() {
    if (state_ == SessionState::Idle || state_ == SessionState::Disconnected) return;
    transport_.Flush();
}

void Session::SetLane(uint16_t firstMsg, uint16_t lastMsg, uint8_t lane) {
    if (lastMsg < firstMsg) std::swap(firstMsg, lastMsg);
    lanes_.push_back({firstMsg, lastMsg, static_cast<uint8_t>(lane < kLanes ? lane : 0)});
}

uint8_t Session::LaneOf(uint16_t msgId) const {
    for (auto it = lanes_.rbegin(); it != lanes_.rend(); ++it)
        if (msgId >= it->first && msgId <= it->last) return it->lane;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// pings and the session clock (docs/protocol.md "Session clock")

double Session::LocalClock() const { return Now() + cfg_.testClockSkewS; }

double Session::SessionTime() const {
    if (state_ == SessionState::Hosting || !clockSynced_) return LocalClock();
    return LocalClock() + clockOffset_;
}

void Session::SendPing(double now) {
    if (pingBurst_ > 0) --pingBurst_;
    const uint32_t stamp = NowMs();
    pingSent_[stamp % pingSent_.size()] = {stamp, now};
    ByteWriter w(8);
    w.U8(kPing);
    w.U32(stamp);
    if (state_ == SessionState::Hosting) {
        for (auto& p : players_)
            if (p.id != kHostPlayer) SendRaw(p.peer, w, Reliability::Unreliable);
    } else if (state_ == SessionState::Connected) {
        SendRaw(hostPeer_, w, Reliability::Unreliable);
    }
}

// Pong: u32 the ping's stamp, then (protocol 9) u64 the answering side's clock in microseconds when it answered.
void Session::OnPong(ByteReader& r, PlayerInfo* from) {
    const uint32_t t = r.U32();
    if (!r.Ok()) return;
    const double recvAt = Now();
    const float rtt = static_cast<float>(NowMs() - t);
    if (from) {
        from->rttMs = from->rttMs <= 0 ? rtt : from->rttMs * 0.8f + rtt * 0.2f;
    } else {
        for (auto& p : players_)
            if (p.id == localId_ || p.id == kHostPlayer) p.rttMs = (p.rttMs <= 0 ? rtt : p.rttMs * 0.8f + rtt * 0.2f);
    }
    if (r.Remaining() < 8 || state_ != SessionState::Connected) return;  // older peer / host: no clock
    const uint64_t remoteUs = r.U64();
    const auto& sent = pingSent_[t % pingSent_.size()];
    if (sent.first != t || sent.second <= 0) return;  // not ours, or overwritten
    ClockSample(sent.second, recvAt, static_cast<double>(remoteUs) / 1e6);
}

// One NTP-like sample: the host's clock read at the middle of the round trip. The offset of the sample with the
// shortest round trip among the recent ones is the estimate (least queueing, least asymmetry), smoothed a little.
void Session::ClockSample(double sentAt, double recvAt, double remoteClock) {
    const double rtt = recvAt - sentAt;
    if (rtt < 0 || rtt > 5.0) return;
    const double skew = cfg_.testClockSkewS;
    const double offset = remoteClock - ((sentAt + recvAt) / 2 + skew);
    clockSamples_[clockNext_] = {offset, rtt, recvAt};
    clockNext_ = (clockNext_ + 1) % clockSamples_.size();
    const ClockSampleRec* best = nullptr;
    for (const auto& s : clockSamples_)
        if (s.at >= 0 && recvAt - s.at <= kClockSampleMaxAge && (!best || s.rtt < best->rtt)) best = &s;
    if (!best) return;
    if (!clockSynced_ || std::fabs(best->offset - clockOffset_) > 0.25) {
        if (clockSynced_)
            Log(kInfo, "session: clock offset jumped %.1f ms (to %.1f ms)", (best->offset - clockOffset_) * 1000,
                best->offset * 1000);
        clockOffset_ = best->offset;
        if (!clockSynced_)
            Log(kInfo, "session: clock synced to the host: offset %.3f s (+-%.1f ms)", best->offset,
                best->rtt * 500);
        clockSynced_ = true;
    } else {
        clockOffset_ += (best->offset - clockOffset_) * 0.3;
    }
    clockErrMs_ = static_cast<float>(best->rtt * 500);
}

void Session::OnEvent(const Event& e) {
    if (state_ == SessionState::Hosting) {
        switch (e.type) {
            case EventType::Connected:
                pending_[e.peer] = Pending{Now(), 0};
                break;
            case EventType::Disconnected:
                if (pending_.erase(e.peer)) break;
                // A clean leave ("closed by remote") is final; a lost connection keeps the slot for a while.
                if (e.reason != DisconnectReason::Remote && e.reason != DisconnectReason::Local &&
                    cfg_.reconnectHoldS > 0 && FindByPeer(e.peer))
                    HostHoldPlayer(e.peer, ToString(e.reason));
                else
                    HostRemovePlayer(e.peer, ToString(e.reason));
                break;
            case EventType::Message: {
                ByteReader r(e.data);
                HostOnMessage(e.peer, r, e.channel);
                break;
            }
        }
        return;
    }
    if (e.peer != hostPeer_) return;
    switch (e.type) {
        case EventType::Connected:
            SendHello();
            break;
        case EventType::Disconnected: {
            if (state_ == SessionState::Reconnecting) {  // this attempt failed; the next one follows the backoff
                hostPeer_ = kInvalidPeer;
                GuestAttemptFailed(ToString(e.reason));
                break;
            }
            if (welcomed_ && e.reason != DisconnectReason::Remote && cfg_.reconnectWindowS > 0) {
                GuestStartReconnect(std::string("connection to host lost: ") + ToString(e.reason));
                break;
            }
            std::string why;
            if (!welcomed_)
                why = std::string("could not connect to host: ") + ToString(e.reason);
            else if (e.reason == DisconnectReason::Remote)
                why = "host closed the session";
            else
                why = std::string("connection to host lost: ") + ToString(e.reason);
            hostPeer_ = kInvalidPeer;
            Fail(why);
            break;
        }
        case EventType::Message: {
            ByteReader r(e.data);
            GuestOnMessage(r);
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// host side

void Session::Reject(PeerId peer, uint8_t code, const std::string& reason) {
    Log(kInfo, "session: rejecting peer %llu: %s", static_cast<unsigned long long>(peer), reason.c_str());
    ByteWriter w(64);
    w.U8(kReject);
    w.U8(code);
    w.Str(reason);
    SendRaw(peer, w, Reliability::Reliable);
    pending_[peer].kickAt = Now() + kRejectLinger;
}

void Session::HostOnHello(PeerId peer, ByteReader& r) {
    uint32_t magic = r.U32();
    uint32_t proto = r.U32();
    uint32_t build = r.U32();
    std::string name = r.Str(256);
    std::string model = r.Str(kMaxModelLen);
    if (!r.Ok() || magic != kHelloMagic) {
        Reject(peer, kRejectBadHello, "invalid handshake");
        return;
    }
    // Optional trailer: u8 hasToken, then the resume token from our Welcome (a reconnecting guest).
    bool hasToken = false;
    ResumeToken token{};
    if (r.Remaining() >= 1 + kResumeTokenSize && r.U8() == 1) hasToken = r.Bytes(token.data(), token.size());
    if (IsBanned(peer)) {
        Reject(peer, kRejectBanned, "You are banned from this session");
        return;
    }
    if (proto != cfg_.protocolVersion || build != cfg_.modBuild) {
        Reject(peer, kRejectVersion,
               "version mismatch: host has protocol " + std::to_string(cfg_.protocolVersion) + " build " +
                   std::to_string(cfg_.modBuild) + ", you have protocol " + std::to_string(proto) + " build " +
                   std::to_string(build) + ". Both players need the same " + cfg_.modName + " version.");
        return;
    }
    // A reconnecting guest with a valid token gets his held slot back (no password: the token proves he was admitted).
    if (hasToken && HostTryResume(peer, token, name, model)) return;
    if (static_cast<int>(players_.size() + held_.size()) >= cfg_.maxPlayers) {
        Reject(peer, kRejectFull, "the session is full (" + std::to_string(cfg_.maxPlayers) + " players)");
        return;
    }
    const bool invited = isInvitedPeer && isInvitedPeer(peer);
    if (!cfg_.password.empty() && !(invited && cfg_.invitesSkipPassword)) {
        if (LockedOut(AttemptKey(peer), Now())) {
            Reject(peer, kRejectTooManyAttempts, "too many attempts, try again later");
            return;
        }
        Pending& pend = pending_[peer];
        if (!auth::SecureRandom(pend.challenge.data(), pend.challenge.size())) {
            Log(kError, "session: no random bytes for the password challenge");
            Reject(peer, kRejectBadHello, "the host could not start the password check");
            return;
        }
        pend.awaitingAuth = true;
        pend.since = Now();  // the Auth must come within helloTimeoutS
        pend.name = std::move(name);
        pend.model = std::move(model);
        ByteWriter w(40);
        w.U8(kChallenge);
        w.Bytes(pend.challenge.data(), pend.challenge.size());
        SendRaw(peer, w, Reliability::Reliable);
        Log(kInfo, "session: password challenge sent to peer %llu", static_cast<unsigned long long>(peer));
        return;
    }
    if (invited && !cfg_.password.empty())
        Log(kInfo, "session: peer %llu is invited; password skipped", static_cast<unsigned long long>(peer));
    HostAdmit(peer, name, model);
}

std::string Session::AttemptKey(PeerId peer) const {
    std::string a = transport_.PeerAddress(peer);
    return a.empty() ? "peer:" + std::to_string(peer) : "addr:" + a;
}

bool Session::LockedOut(const std::string& key, double now) {
    auto it = authFailures_.find(key);
    if (it == authFailures_.end()) return false;
    auto& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(), [&](double t) { return now - t > cfg_.authWindowS; }), v.end());
    if (v.empty()) {
        authFailures_.erase(it);
        return false;
    }
    return static_cast<int>(v.size()) >= cfg_.authMaxFailures;
}

void Session::HostOnAuth(PeerId peer, ByteReader& r) {
    const bool has = r.U8() != 0;
    uint8_t mac[auth::kMacSize] = {};
    if (has) r.Bytes(mac, sizeof(mac));
    if (!r.Ok()) {
        Reject(peer, kRejectBadHello, "invalid handshake");
        return;
    }
    const std::string key = AttemptKey(peer);
    const double now = Now();
    if (LockedOut(key, now)) {
        Reject(peer, kRejectTooManyAttempts, "too many attempts, try again later");
        return;
    }
    if (!has) {
        Reject(peer, kRejectPasswordRequired, "password required");
        return;
    }
    Pending& pend = pending_[peer];
    uint8_t want[auth::kMacSize];
    const bool computed = auth::Response(cfg_.password, pend.challenge.data(), want);
    if (computed && auth::ConstantTimeEqual(mac, want, sizeof(want))) {
        Log(kInfo, "session: peer %llu passed the password check", static_cast<unsigned long long>(peer));
        const std::string name = pend.name, model = pend.model;
        HostAdmit(peer, name, model);
        return;
    }
    if (authFailures_.size() >= kMaxAuthKeys) {
        // Many addresses failed: forget those whose failures are all outside the window (LockedOut prunes only the
        // key it looks at, so without this the map would grow with every new address).
        for (auto it = authFailures_.begin(); it != authFailures_.end();) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [&](double t) { return now - t > cfg_.authWindowS; }), v.end());
            it = v.empty() ? authFailures_.erase(it) : std::next(it);
        }
    }
    auto& fails = authFailures_[key];
    fails.push_back(now);
    Log(kInfo, "session: wrong password from peer %llu (%s, %zu recent failures)",
        static_cast<unsigned long long>(peer), key.c_str(), fails.size());
    // Answer later, without blocking: Poll sends the Reject at rejectAt.
    pend.rejectAt = now + cfg_.authFailDelayS;
    pend.rejectCode = kRejectWrongPassword;
    pend.rejectReason = "wrong password";
}

void Session::HostAdmit(PeerId peer, const std::string& name, const std::string& model, const Held* resume) {
    PlayerId id = kNoPlayer;
    if (resume) {
        id = resume->info.id;  // his held slot (already taken out of held_ by the caller)
    } else {
        if (static_cast<int>(players_.size() + held_.size()) >= cfg_.maxPlayers) {
            Reject(peer, kRejectFull, "the session is full (" + std::to_string(cfg_.maxPlayers) + " players)");
            return;
        }
        auto heldId = [&](PlayerId c) {
            for (const Held& h : held_)
                if (h.info.id == c) return true;
            return false;
        };
        for (PlayerId c = 1; c < cfg_.maxPlayers; ++c)
            if (!FindPlayer(c) && !heldId(c)) {
                id = c;
                break;
            }
        if (id == kNoPlayer) {
            Reject(peer, kRejectFull, "the session is full");
            return;
        }
    }
    pending_.erase(peer);
    transport_.MarkEstablished(peer);  // a real player now: the transport never evicts it to make room
    PlayerInfo info{id, CleanName(name), CleanModel(model), peer, 0, ++joinCounter_};
    players_.push_back(info);
    std::sort(players_.begin(), players_.end(), [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
    // A fresh resume token for every admit (also a resume): what he sends back if his connection is lost.
    GuestSecret& sec = secrets_[id];
    sec = GuestSecret{};
    const bool haveToken = auth::SecureRandom(sec.token.data(), sec.token.size());
    if (!haveToken) Log(kWarn, "session: no random bytes for player %u's resume token: he cannot reconnect", id);
    sec.address = transport_.PeerAddress(peer);
    sec.steamId = transport_.PeerSteamId(peer);

    ByteWriter w(128);
    w.U8(kWelcome);
    w.U8(id);
    w.U8(static_cast<uint8_t>(cfg_.maxPlayers));
    w.U8(static_cast<uint8_t>(players_.size()));
    for (auto& p : players_) {
        w.U8(p.id);
        w.Str(p.name);
        w.Str(p.model);
    }
    w.U8(static_cast<uint8_t>((resume ? kWelcomeResumed : 0) | (haveToken ? kWelcomeHasToken : 0)));
    if (haveToken) w.Bytes(sec.token.data(), sec.token.size());
    SendRaw(peer, w, Reliability::Reliable);

    ByteWriter j(64);
    j.U8(kPeerJoined);
    j.U8(id);
    j.Str(info.name);
    j.Str(info.model);
    BroadcastRaw(j, Reliability::Reliable, id);
    if (resume) {
        Log(kInfo, "session: player %u '%s' is back after a lost connection (peer %llu, slot held %.1f s)", id,
            info.name.c_str(), static_cast<unsigned long long>(peer), cfg_.reconnectHoldS - (resume->until - Now()));
        if (onPlayerResumed)
            onPlayerResumed(info);
        else if (onPlayerJoined)
            onPlayerJoined(info);
        return;
    }
    Log(kInfo, "session: player %u '%s' joined (peer %llu)", id, info.name.c_str(), static_cast<unsigned long long>(peer));
    if (onPlayerJoined) onPlayerJoined(info);
}

// Host: a guest's connection was lost (timeout / transport error): keep his slot for reconnectHoldS. He leaves
// Players() (the others get PeerDropped and drop his puppet); a Hello with his resume token gives the slot back.
void Session::HostHoldPlayer(PeerId peer, const std::string& reason) {
    auto it = std::find_if(players_.begin(), players_.end(),
                           [&](const PlayerInfo& p) { return p.peer == peer && p.id != kHostPlayer; });
    if (it == players_.end()) return;
    Held h;
    h.info = *it;
    h.info.peer = kInvalidPeer;
    h.until = Now() + cfg_.reconnectHoldS;
    if (auto s = secrets_.find(h.info.id); s != secrets_.end()) h.secret = s->second;
    players_.erase(it);
    if (rateLogPending_) LogRateLimits(-1);
    limits_.erase(h.info.id);
    const std::string why = CleanText(reason, kMaxReasonLen);
    ByteWriter w(64);
    w.U8(kPeerDropped);
    w.U8(h.info.id);
    w.U16(static_cast<uint16_t>(std::clamp(cfg_.reconnectHoldS, 0.0, 65535.0)));
    w.Str(why);
    BroadcastRaw(w, Reliability::Reliable, kNoPlayer);
    Log(kInfo, "session: player %u '%s' lost the connection (%s); slot held for %.0f s", h.info.id,
        h.info.name.c_str(), why.c_str(), cfg_.reconnectHoldS);
    const PlayerInfo info = h.info;
    held_.push_back(std::move(h));
    if (onPlayerDropped) onPlayerDropped(info, why);
}

void Session::HostExpireHolds(double now) {
    for (size_t i = 0; i < held_.size();) {
        if (held_[i].until > now) {
            ++i;
            continue;
        }
        const PlayerInfo info = held_[i].info;
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        secrets_.erase(info.id);
        ByteWriter w(64);
        w.U8(kPeerLeft);
        w.U8(info.id);
        w.Str(kHoldExpiredReason);
        BroadcastRaw(w, Reliability::Reliable, kNoPlayer);
        Log(kInfo, "session: player %u '%s' did not reconnect within %.0f s; slot freed", info.id, info.name.c_str(),
            cfg_.reconnectHoldS);
        if (onPlayerLeft) onPlayerLeft(info, kHoldExpiredReason);
    }
}

// Host: Hello with a resume token. A held slot -> back in it; a player still connected (his old connection has not
// timed out here yet) -> the old connection is dropped and he resumes on the new one. False: unknown token (the hold
// expired, or another session's token): the Hello goes on as a normal join.
bool Session::HostTryResume(PeerId peer, const ResumeToken& token, const std::string& name, const std::string& model) {
    auto match = [&](const ResumeToken& t) { return auth::ConstantTimeEqual(t.data(), token.data(), token.size()); };
    for (size_t i = 0; i < held_.size(); ++i) {
        if (!match(held_[i].secret.token)) continue;
        Held h = std::move(held_[i]);
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        Log(kInfo, "session: peer %llu has player %u's resume token: giving the held slot back",
            static_cast<unsigned long long>(peer), h.info.id);
        HostAdmit(peer, name, model, &h);
        return true;
    }
    for (const PlayerInfo& p : players_) {
        if (p.id == kHostPlayer || p.peer == peer) continue;
        auto s = secrets_.find(p.id);
        if (s == secrets_.end() || !match(s->second.token)) continue;
        const PeerId old = p.peer;
        const PlayerId id = p.id;
        Log(kInfo,
            "session: player %u reconnected on a new connection (peer %llu) before his old one (peer %llu) timed out "
            "here: dropping the old one",
            id, static_cast<unsigned long long>(peer), static_cast<unsigned long long>(old));
        transport_.Disconnect(old);
        HostHoldPlayer(old, "reconnected on a new connection");
        return HostTryResume(peer, token, name, model);  // now held: resumes above
    }
    Log(kInfo, "session: peer %llu sent an unknown resume token (slot given up?): joining as a new player",
        static_cast<unsigned long long>(peer));
    return false;
}

void Session::HostRemovePlayer(PeerId peer, const std::string& reason) {
    auto it = std::find_if(players_.begin(), players_.end(),
                           [&](const PlayerInfo& p) { return p.peer == peer && p.id != kHostPlayer; });
    if (it == players_.end()) return;
    PlayerInfo info = *it;
    players_.erase(it);
    if (rateLogPending_) LogRateLimits(-1);  // flush what this player had dropped before the id is reused
    limits_.erase(info.id);
    secrets_.erase(info.id);
    ByteWriter w(64);
    w.U8(kPeerLeft);
    w.U8(info.id);
    w.Str(reason);
    BroadcastRaw(w, Reliability::Reliable, kNoPlayer);
    Log(kInfo, "session: player %u '%s' left: %s", info.id, info.name.c_str(), reason.c_str());
    if (onPlayerLeft) onPlayerLeft(info, reason);
}

// ---------------------------------------------------------------------------------------------------------------
// kick / ban (host)

bool Session::Kick(PlayerId id, std::string_view reason) { return KickImpl(id, reason, false); }
bool Session::KickAndBan(PlayerId id, std::string_view reason) { return KickImpl(id, reason, true); }

bool Session::KickImpl(PlayerId id, std::string_view reasonIn, bool ban) {
    if (state_ != SessionState::Hosting || id == kHostPlayer) return false;
    const PlayerInfo* p = FindPlayer(id);
    if (!p) {
        // A held (lost) player: no connection to tell; free his slot (and ban what he had) right away.
        auto h = std::find_if(held_.begin(), held_.end(), [&](const Held& x) { return x.info.id == id; });
        if (h == held_.end()) return false;
        const Held gone = *h;
        held_.erase(h);
        secrets_.erase(id);
        const std::string reason = CleanText(std::string(reasonIn), kMaxReasonLen);
        if (ban && (!gone.secret.address.empty() || gone.secret.steamId)) {
            Log(kInfo, "session: banned '%s' (address '%s', SteamID %llu) while he was reconnecting",
                gone.info.name.c_str(), gone.secret.address.c_str(),
                static_cast<unsigned long long>(gone.secret.steamId));
            bans_.push_back({gone.info.name, gone.secret.address, gone.secret.steamId});
        }
        std::string left = ban ? "kicked and banned by the host" : "kicked by the host";
        if (!reason.empty()) left += ": " + reason;
        left = CleanText(left, kMaxReasonLen);
        ByteWriter w(64);
        w.U8(kPeerLeft);
        w.U8(id);
        w.Str(left);
        BroadcastRaw(w, Reliability::Reliable, kNoPlayer);
        Log(kInfo, "session: held player %u '%s' %s", id, gone.info.name.c_str(), left.c_str());
        if (onPlayerLeft) onPlayerLeft(gone.info, left);
        return true;
    }
    const PeerId peer = p->peer;
    const std::string name = p->name;
    const std::string reason = CleanText(std::string(reasonIn), kMaxReasonLen);
    if (ban) {
        BanEntry b{name, transport_.PeerAddress(peer), transport_.PeerSteamId(peer)};
        if (b.address.empty() && b.steamId == 0) {
            Log(kWarn, "session: player %u '%s' has no address or SteamID; the ban cannot match a rejoin", id,
                name.c_str());
        } else {
            Log(kInfo, "session: banned '%s' (address '%s', SteamID %llu)", name.c_str(), b.address.c_str(),
                static_cast<unsigned long long>(b.steamId));
            bans_.push_back(std::move(b));
        }
    }
    Log(kInfo, "session: kicking player %u '%s'%s%s%s", id, name.c_str(), ban ? " with a ban" : "",
        reason.empty() ? "" : ": ", reason.c_str());
    ByteWriter w(reason.size() + 8);
    w.U8(kKicked);
    w.U8(ban ? kKickBanned : kKickOnly);
    w.Str(reason);
    SendRaw(peer, w, Reliability::Reliable);
    std::string left = ban ? "kicked and banned by the host" : "kicked by the host";
    if (!reason.empty()) left += ": " + reason;
    HostRemovePlayer(peer, CleanText(left, kMaxReasonLen));  // PeerLeft to the others, onPlayerLeft
    // Like a Reject: keep the connection for kRejectLinger so the notice is delivered, ignore what it sends, then
    // drop it (Poll). The guest normally disconnects first when the notice arrives.
    Pending& pend = pending_[peer];
    pend = Pending{};
    pend.since = Now();
    pend.kickAt = pend.since + kRejectLinger;
    return true;
}

bool Session::IsBanned(PeerId peer) const {
    if (bans_.empty()) return false;
    const std::string address = transport_.PeerAddress(peer);
    const uint64_t steamId = transport_.PeerSteamId(peer);
    for (const BanEntry& b : bans_)
        if ((!address.empty() && b.address == address) || (steamId != 0 && b.steamId == steamId)) return true;
    return false;
}

bool Session::Unban(size_t index) {
    if (index >= bans_.size()) return false;
    Log(kInfo, "session: unbanned '%s'", bans_[index].name.c_str());
    bans_.erase(bans_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void Session::HostOnMessage(PeerId peer, ByteReader& r, Reliability ch) {
    uint8_t type = r.U8();
    if (!r.Ok()) return;
    auto pend = pending_.find(peer);
    if (pend != pending_.end()) {
        if (pend->second.kickAt > 0 || pend->second.rejectAt > 0) return;  // already rejected
        if (!pend->second.awaitingAuth && type == kHello)
            HostOnHello(peer, r);
        else if (pend->second.awaitingAuth && type == kAuth)
            HostOnAuth(peer, r);
        else
            Reject(peer, kRejectBadHello, "invalid handshake");
        return;
    }
    PlayerInfo* from = FindByPeer(peer);
    if (!from) return;
    if (type == kHello) {
        // A Hello on a connection that already finished its handshake: the guest lost us and is reconnecting while
        // the transport kept (or re-opened) the same connection here, e.g. Steam P2P where the peer id is his SteamID
        // and his CONNECTs keep the old connection alive. Treat it as a lost connection plus a new join on it.
        Log(kInfo, "session: player %u sent a new Hello on his existing connection: he is reconnecting", from->id);
        HostHoldPlayer(peer, "reconnecting");
        pending_[peer] = Pending{Now(), 0};
        HostOnHello(peer, r);
        return;
    }
    switch (type) {
        case kPing: {
            uint32_t t = r.U32();
            if (!r.Ok()) return;
            ByteWriter w(16);
            w.U8(kPong);
            w.U32(t);
            w.U64(static_cast<uint64_t>(std::max(0.0, LocalClock()) * 1e6));
            SendRaw(peer, w, Reliability::Unreliable);
            break;
        }
        case kPong:
            OnPong(r, from);
            break;
        case kChat: {
            r.U8();  // claimed sender, ignored
            std::string text = r.Str(kMaxChatLen);
            if (!r.Ok()) return;
            PlayerId fid = from->id;
            if (cfg_.chatPerS > 0 &&
                !limits_[fid].chat.Take(1.0, cfg_.chatPerS, std::max(cfg_.chatBurst, 1.0), Now())) {
                CountDropped(*from, true, 0);
                return;
            }
            ByteWriter w(text.size() + 8);
            w.U8(kChat);
            w.U8(fid);
            w.Str(text);
            BroadcastRaw(w, Reliability::Reliable, fid);
            if (onChat) onChat(fid, text);
            break;
        }
        case kApp: {
            uint16_t id = static_cast<uint16_t>(r.VarU32());
            r.U8();  // claimed sender, replaced by the real one
            PlayerId target = r.U8();
            uint8_t flags = r.U8();
            const bool stamped = (flags & kAppFlagStamped) != 0;
            const uint32_t stampMs = stamped ? r.U32() : 0;
            auto payload = r.Rest();
            if (!r.Ok()) return;
            PlayerId fid = from->id;
            Reliability rel = (flags & kAppFlagReliable) ? Reliability::Reliable : ch;
            // Relayed messages (to another guest or everyone) cost the sender's relay budget; over it the whole
            // message is dropped, so host and guests see the same thing.
            if (target != kHostPlayer && cfg_.relayBytesPerS > 0 &&
                !limits_[fid].relay.Take(static_cast<double>(payload.size()), cfg_.relayBytesPerS,
                                         std::max(cfg_.relayBurstBytes, 1.0), Now())) {
                CountDropped(*from, false, payload.size());
                return;
            }
            {
                MsgTraffic& t = traffic_[id];
                ++t.recvMsgs;
                t.recvBytes += r.Position();
            }
            if (target == kHostPlayer || target == kAllPlayers) DispatchApp(id, fid, payload, stamped, stampMs);
            if (target != kHostPlayer) {
                ByteWriter w = AppHeader(id, fid, target, rel, stamped, stampMs);
                w.Bytes(payload);
                int copies = 0, failed = 0;
                const uint8_t lane = LaneOf(id);
                if (target == kAllPlayers) {
                    BroadcastRaw(w, rel, fid, lane);
                    copies = lastCopies_;
                    failed = lastFailed_;
                } else if (PlayerInfo* to = FindMutable(target); to && to->id != kHostPlayer && to->id != fid) {
                    copies = 1;
                    failed = SendRaw(to->peer, w, rel, lane) ? 0 : 1;
                }
                MsgTraffic& t = traffic_[id];
                t.relayedMsgs += static_cast<uint64_t>(copies - failed);
                t.relayedBytes += static_cast<uint64_t>(copies - failed) * w.Size();
                t.sendFailed += static_cast<uint64_t>(failed);
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// rate limits (host)

bool Session::Bucket::Take(double cost, double rate, double burst, double now) {
    if (!started) {
        started = true;
        tokens = burst;
    } else {
        tokens = std::min(burst, tokens + (now - last) * rate);
    }
    last = now;
    if (tokens <= 0) return false;
    tokens -= cost;  // may go below 0 for one large message: then it has to refill first
    if (tokens < 0) tokens = std::max(tokens, -burst);
    return true;
}

void Session::CountDropped(PlayerInfo& from, bool chat, size_t bytes) {
    GuestLimits& g = limits_[from.id];
    if (chat) {
        ++g.chatDropped;
        ++rateStats_.chatDropped;
    } else {
        ++g.appDropped;
        g.appBytesDropped += bytes;
        ++rateStats_.appDropped;
        rateStats_.appBytesDropped += bytes;
    }
    rateLogPending_ = true;
    LogRateLimits(Now());
}

// At most one log line per second (per guest that dropped something since the last one). now < 0: log right away.
void Session::LogRateLimits(double now) {
    if (now >= 0 && now - lastRateLog_ < 1.0) return;
    if (now >= 0) lastRateLog_ = now;
    rateLogPending_ = false;
    for (auto& [id, g] : limits_) {
        if (!g.chatDropped && !g.appDropped) continue;
        const PlayerInfo* p = FindPlayer(id);
        Log(kWarn, "session: rate limit: player %u '%s': dropped %u chat, %u relayed app messages (%llu bytes)", id,
            p ? p->name.c_str() : "?", g.chatDropped, g.appDropped, static_cast<unsigned long long>(g.appBytesDropped));
        g.chatDropped = g.appDropped = 0;
        g.appBytesDropped = 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// guest side

void Session::GuestOnMessage(ByteReader& r) {
    uint8_t type = r.U8();
    if (!r.Ok()) return;
    if (!welcomed_) {
        if (type == kWelcome) {
            GuestOnWelcome(r);
        } else if (type == kChallenge) {
            uint8_t challenge[auth::kChallengeSize];
            if (!r.Bytes(challenge, sizeof(challenge))) {
                Fail("invalid password challenge from host");
                return;
            }
            ByteWriter w(40);
            w.U8(kAuth);
            uint8_t mac[auth::kMacSize];
            if (!cfg_.password.empty() && auth::Response(cfg_.password, challenge, mac)) {
                w.U8(1);
                w.Bytes(mac, sizeof(mac));
            } else {
                w.U8(0);  // no password: the host answers "password required"
            }
            SendRaw(hostPeer_, w, Reliability::Reliable);
            Log(kInfo, "session: host asked for a password (%s)", cfg_.password.empty() ? "we have none" : "answered");
        } else if (type == kReject) {
            const uint8_t code = r.U8();
            std::string reason = r.Str(kMaxReasonLen);
            lastReject_ = code >= 1 && code <= 7 ? static_cast<RejectReason>(code) : RejectReason::None;
            Fail("rejected by host: " + (reason.empty() ? std::string("no reason given") : reason));
        }
        return;
    }
    switch (type) {
        case kPeerJoined: {
            PlayerInfo p;
            p.id = r.U8();
            p.name = CleanName(r.Str(kMaxNameLen * 4));
            p.model = CleanModel(r.Str(kMaxModelLen));
            if (!r.Ok() || p.id >= kMaxPlayers || FindPlayer(p.id)) return;
            p.joinSeq = ++joinCounter_;
            bool resumed = false;
            for (auto d = droppedOthers_.begin(); d != droppedOthers_.end(); ++d)
                if (d->first.id == p.id) {
                    droppedOthers_.erase(d);
                    resumed = true;
                    break;
                }
            players_.push_back(p);
            std::sort(players_.begin(), players_.end(),
                      [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
            if (resumed) Log(kInfo, "session: player %u '%s' is back", p.id, p.name.c_str());
            if (resumed && onPlayerResumed)
                onPlayerResumed(p);
            else if (onPlayerJoined)
                onPlayerJoined(p);
            break;
        }
        case kPeerDropped: {
            PlayerId id = r.U8();
            const uint16_t holdS = r.U16();
            std::string reason = CleanText(r.Str(kMaxReasonLen), kMaxReasonLen);
            if (!r.Ok() || id == localId_ || id == kHostPlayer) return;
            auto it = std::find_if(players_.begin(), players_.end(), [&](const PlayerInfo& p) { return p.id == id; });
            if (it == players_.end()) return;
            PlayerInfo info = *it;
            players_.erase(it);
            droppedOthers_.push_back({info, Now() + holdS});
            Log(kInfo, "session: player %u '%s' lost the connection (%s); the host holds the slot for %u s", id,
                info.name.c_str(), reason.c_str(), holdS);
            if (onPlayerDropped) onPlayerDropped(info, reason);
            break;
        }
        case kPeerLeft: {
            PlayerId id = r.U8();
            std::string reason = r.Str(kMaxReasonLen);
            if (!r.Ok() || id == localId_ || id == kHostPlayer) return;  // we and the host never "leave" by message
            auto it = std::find_if(players_.begin(), players_.end(), [&](const PlayerInfo& p) { return p.id == id; });
            if (it == players_.end()) {
                // a dropped player who did not come back (or was kicked while gone)
                for (auto d = droppedOthers_.begin(); d != droppedOthers_.end(); ++d)
                    if (d->first.id == id) {
                        PlayerInfo info = d->first;
                        droppedOthers_.erase(d);
                        if (onPlayerLeft) onPlayerLeft(info, reason);
                        break;
                    }
                return;
            }
            PlayerInfo info = *it;
            players_.erase(it);
            if (onPlayerLeft) onPlayerLeft(info, reason);
            break;
        }
        case kKicked: {
            const uint8_t code = r.U8();
            const std::string reason = CleanText(r.Str(kMaxReasonLen), kMaxReasonLen);
            if (!r.Ok()) return;
            kicked_ = code == kKickBanned ? 2 : 1;
            std::string why = kicked_ == 2 ? "You were kicked and banned by the host" : "You were kicked by the host";
            if (!reason.empty()) why += ": " + reason;
            Fail(why);  // disconnects from the host; onDisconnected(why), WasKicked()
            return;
        }
        case kPing: {
            uint32_t t = r.U32();
            if (!r.Ok()) return;
            ByteWriter w(16);
            w.U8(kPong);
            w.U32(t);
            w.U64(static_cast<uint64_t>(std::max(0.0, LocalClock()) * 1e6));
            SendRaw(hostPeer_, w, Reliability::Unreliable);
            break;
        }
        case kPong:
            OnPong(r, nullptr);
            break;
        case kPingTable: {
            uint8_t n = r.U8();
            for (int i = 0; i < n && r.Ok(); ++i) {
                PlayerId id = r.U8();
                uint16_t ms = r.U16();
                if (!r.Ok()) break;
                if (id == localId_ || id == kHostPlayer) continue;  // we measure those ourselves
                if (PlayerInfo* p = FindMutable(id)) p->rttMs = ms;
            }
            break;
        }
        case kChat: {
            PlayerId from = r.U8();
            std::string text = r.Str(kMaxChatLen);
            if (r.Ok() && onChat) onChat(from, text);
            break;
        }
        case kApp: {
            uint16_t id = static_cast<uint16_t>(r.VarU32());
            PlayerId from = r.U8();
            r.U8();  // target
            const uint8_t flags = r.U8();
            const bool stamped = (flags & kAppFlagStamped) != 0;
            const uint32_t stampMs = stamped ? r.U32() : 0;
            auto payload = r.Rest();
            if (r.Ok()) {
                MsgTraffic& t = traffic_[id];
                ++t.recvMsgs;
                t.recvBytes += r.Position();
                DispatchApp(id, from, payload, stamped, stampMs);
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// guest: handshake and reconnect (docs/protocol.md "Reconnect")

void Session::SendHello() {
    ByteWriter w(96);
    w.U8(kHello);
    w.U32(kHelloMagic);
    w.U32(cfg_.protocolVersion);
    w.U32(cfg_.modBuild);
    w.Str(cfg_.playerName);
    w.Str(CleanModel(cfg_.playerModel));
    const bool resume = state_ == SessionState::Reconnecting && hasResumeToken_;
    if (resume) {  // trailer: we were in this session; the token asks for our held slot
        w.U8(1);
        w.Bytes(resumeToken_.data(), resumeToken_.size());
    }
    SendRaw(hostPeer_, w, Reliability::Reliable);
    helloSentAt_ = Now();
    if (state_ == SessionState::Reconnecting)
        Log(kInfo, "session: reconnect attempt %d: connected, Hello sent%s", reconnectAttempts_,
            resume ? " with our resume token" : " (no resume token: joining as a new player)");
}

void Session::GuestOnWelcome(ByteReader& r) {
    PlayerId me = r.U8();
    r.U8();  // max players
    uint8_t n = r.U8();
    std::vector<PlayerInfo> list;
    bool valid = me != kHostPlayer && me < kMaxPlayers;
    bool haveMe = false, haveHost = false;
    for (int i = 0; i < n && r.Ok(); ++i) {
        PlayerInfo p;
        p.id = r.U8();
        p.name = CleanName(r.Str(kMaxNameLen * 4));
        p.model = CleanModel(r.Str(kMaxModelLen));
        p.peer = p.id == kHostPlayer ? hostPeer_ : kInvalidPeer;
        if (p.id >= kMaxPlayers) valid = false;
        for (const PlayerInfo& q : list)
            if (q.id == p.id) valid = false;
        haveMe |= p.id == me;
        haveHost |= p.id == kHostPlayer;
        list.push_back(std::move(p));
    }
    if (!r.Ok() || !valid || !haveMe || !haveHost) {
        Fail("invalid welcome from host");
        return;
    }
    // Trailer (an older host sends none): u8 flags, then the resume token if flag 2.
    uint8_t flags = 0;
    if (r.Remaining() >= 1) flags = r.U8();
    hasResumeToken_ = false;
    if ((flags & kWelcomeHasToken) && r.Remaining() >= kResumeTokenSize)
        hasResumeToken_ = r.Bytes(resumeToken_.data(), resumeToken_.size());
    for (PlayerInfo& p : list) p.joinSeq = ++joinCounter_;
    players_ = std::move(list);
    welcomed_ = true;
    const bool reconnect = state_ == SessionState::Reconnecting;
    state_ = SessionState::Connected;
    pingBurst_ = kPingBurst;  // sync the session clock now (and measure the round trip)
    lastPing_ = 0;
    if (!reconnect) {
        localId_ = me;
        Log(kInfo, "session: joined as player %u (%zu players)", me, players_.size());
        if (onJoined) onJoined();
        return;
    }
    const bool sameSlot = (flags & kWelcomeResumed) && me == reconnectOldId_;
    localId_ = me;
    attemptOpen_ = false;
    droppedOthers_.clear();  // the Welcome's list is the truth now
    Log(kInfo, "session: reconnected after %.1f s (%d attempt(s)): %s player %u (%zu players)",
        cfg_.reconnectWindowS - (reconnectUntil_ - Now()), reconnectAttempts_,
        sameSlot ? "same slot, still" : "slot was given up, now", me, players_.size());
    reconnectReason_.clear();
    if (onReconnected) onReconnected(sameSlot);
}

// The connection to the host was lost (not a clean close, not a kick): keep the session and rejoin with backoff.
// Players() keeps only us (the others' puppets go; they come back with the host's Welcome), LocalId() stays.
void Session::GuestStartReconnect(const std::string& reason) {
    const double now = Now();
    Log(kWarn, "session: %s; reconnecting to %s for up to %.0f s (%s)", reason.c_str(), lastJoinAddress_.c_str(),
        cfg_.reconnectWindowS, hasResumeToken_ ? "with our resume token" : "no resume token: as a new player");
    reconnectReason_ = reason;
    reconnectOldId_ = localId_;
    PlayerInfo self;
    if (const PlayerInfo* me = FindPlayer(localId_)) self = *me;
    self.id = localId_;
    self.peer = kInvalidPeer;
    self.rttMs = 0;
    players_.clear();
    players_.push_back(self);
    droppedOthers_.clear();
    welcomed_ = false;
    hostPeer_ = kInvalidPeer;
    attemptOpen_ = false;
    helloSentAt_ = 0;
    transport_.Close();
    state_ = SessionState::Reconnecting;
    reconnectUntil_ = now + cfg_.reconnectWindowS;
    reconnectDelay_ = std::max(0.05, cfg_.reconnectFirstDelayS);
    reconnectNextAt_ = now + reconnectDelay_;
    reconnectAttempts_ = 0;
    if (onConnectionLost) onConnectionLost(reason);
}

void Session::GuestAttemptFailed(const char* why) {
    Log(kInfo, "session: reconnect attempt %d failed (%s); next in %.1f s, %.0f s left", reconnectAttempts_, why,
        std::min(reconnectDelay_ * 2, std::max(cfg_.reconnectMaxDelayS, 0.05)), ReconnectLeftS());
    if (hostPeer_) transport_.Disconnect(hostPeer_);
    hostPeer_ = kInvalidPeer;
    attemptOpen_ = false;
    welcomed_ = false;
    helloSentAt_ = 0;
    transport_.Close();  // the next attempt starts from a fresh socket (UDP: a new source port)
    reconnectDelay_ = std::min(reconnectDelay_ * 2, std::max(cfg_.reconnectMaxDelayS, 0.05));
    reconnectNextAt_ = Now() + reconnectDelay_;
}

void Session::GuestReconnectPoll(double now) {
    if (now >= reconnectUntil_) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " (could not reconnect within %.0f s, %d attempt(s))", cfg_.reconnectWindowS,
                      reconnectAttempts_);
        const std::string why = reconnectReason_ + buf;
        attemptOpen_ = false;
        Fail(why);  // onDisconnected: the session is over
        return;
    }
    if (attemptOpen_) {
        if (now - attemptStartedAt_ > cfg_.reconnectAttemptS) GuestAttemptFailed("no answer");
        return;
    }
    if (now < reconnectNextAt_) return;
    ++reconnectAttempts_;
    transport_.Close();
    hostPeer_ = transport_.Connect(lastJoinAddress_);
    if (!hostPeer_) {
        GuestAttemptFailed("the transport could not start a connection");
        return;
    }
    attemptOpen_ = true;
    attemptStartedAt_ = now;
    welcomed_ = false;
    helloSentAt_ = 0;
    Log(kInfo, "session: reconnect attempt %d to %s over %s (%.0f s left)", reconnectAttempts_,
        lastJoinAddress_.c_str(), transport_.Name(), reconnectUntil_ - now);
}

double Session::ReconnectLeftS() const {
    return state_ == SessionState::Reconnecting ? std::max(0.0, reconnectUntil_ - Now()) : 0.0;
}

std::vector<DroppedPlayer> Session::DroppedPlayers() const {
    std::vector<DroppedPlayer> out;
    const double now = Now();
    for (const Held& h : held_) out.push_back({h.info, std::max(0.0, h.until - now)});
    for (const auto& [info, until] : droppedOthers_) out.push_back({info, std::max(0.0, until - now)});
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// app messages

void Session::On(uint16_t msgId, AppHandler handler) { handlers_[msgId] = std::move(handler); }
void Session::Off(uint16_t msgId) { handlers_.erase(msgId); }

void Session::DispatchApp(uint16_t id, PlayerId from, std::span<const uint8_t> payload, bool stamped,
                          uint32_t stampMs) {
    float age = -1;
    if (from == localId_) {
        age = 0;  // our own (host loopback)
        stampMs = SessionTimeMs();
    } else if (stamped && ClockSynced()) {
        const int32_t d = static_cast<int32_t>(SessionTimeMs() - stampMs);
        if (d > -1000 && d < 60000) age = static_cast<float>(std::max(0, d));  // clocks a little off: 0
    }
    if (age >= 0 && from != localId_) {
        MsgTraffic& t = traffic_[id];
        ++t.agedMsgs;
        t.ageSumMs += age;
        t.ageMaxMs = std::max(t.ageMaxMs, age);
    }
    auto it = handlers_.find(id);
    if (it == handlers_.end()) return;
    ByteReader r(payload);
    const float prevAge = curAgeMs_;
    const uint32_t prevStamp = curStampMs_;
    curAgeMs_ = age;
    curStampMs_ = stampMs;
    it->second(from, r);
    curAgeMs_ = prevAge;
    curStampMs_ = prevStamp;
}

ByteWriter Session::AppHeader(uint16_t id, PlayerId from, PlayerId target, Reliability rel, bool haveStamp,
                              uint32_t stampMs) const {
    if (!haveStamp && ClockSynced()) {
        haveStamp = true;
        stampMs = SessionTimeMs();
    }
    ByteWriter w(20);
    w.U8(kApp);
    w.VarU32(id);
    w.U8(from);
    w.U8(target);
    w.U8(static_cast<uint8_t>((rel == Reliability::Reliable ? kAppFlagReliable : 0) |
                              (haveStamp ? kAppFlagStamped : 0)));
    if (haveStamp) w.U32(stampMs);
    return w;
}

bool Session::SendToHost(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel) {
    if (state_ == SessionState::Hosting) {
        DispatchApp(msgId, kHostPlayer, payload);
        return true;
    }
    if (state_ != SessionState::Connected) return false;
    ByteWriter w = AppHeader(msgId, localId_, kHostPlayer, rel);
    w.Bytes(payload);
    const bool ok = SendRaw(hostPeer_, w, rel, LaneOf(msgId));
    CountSent(msgId, w.Size(), 1, ok ? 0 : 1);
    return ok;
}

bool Session::Broadcast(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel) {
    if (!IsActive()) return false;
    ByteWriter w = AppHeader(msgId, localId_, kAllPlayers, rel);
    w.Bytes(payload);
    if (state_ == SessionState::Hosting) {
        const bool ok = BroadcastRaw(w, rel, kHostPlayer, LaneOf(msgId));
        CountSent(msgId, w.Size(), lastCopies_, lastFailed_);
        return ok;
    }
    const bool ok = SendRaw(hostPeer_, w, rel, LaneOf(msgId));
    CountSent(msgId, w.Size(), 1, ok ? 0 : 1);
    return ok;
}

bool Session::SendTo(PlayerId player, uint16_t msgId, std::span<const uint8_t> payload, Reliability rel) {
    if (!IsActive()) return false;
    if (player == localId_) {
        DispatchApp(msgId, localId_, payload);
        return true;
    }
    ByteWriter w = AppHeader(msgId, localId_, player, rel);
    w.Bytes(payload);
    bool ok;
    if (state_ == SessionState::Hosting) {
        PlayerInfo* p = FindMutable(player);
        ok = p && SendRaw(p->peer, w, rel, LaneOf(msgId));
    } else {
        ok = SendRaw(hostPeer_, w, rel, LaneOf(msgId));  // host delivers (player 0) or relays
    }
    CountSent(msgId, w.Size(), 1, ok ? 0 : 1);
    return ok;
}

void Session::CountSent(uint16_t id, size_t bytes, int copies, int failed) {
    MsgTraffic& t = traffic_[id];
    const auto okCopies = static_cast<uint64_t>(std::max(0, copies - failed));
    t.sentMsgs += okCopies;
    t.sentBytes += okCopies * bytes;
    t.sendFailed += static_cast<uint64_t>(std::max(0, failed));
}

bool Session::SendChat(std::string_view text) {
    if (!IsActive()) return false;
    std::string t(text.substr(0, kMaxChatLen));
    ByteWriter w(t.size() + 8);
    w.U8(kChat);
    w.U8(localId_);
    w.Str(t);
    if (state_ == SessionState::Hosting) return BroadcastRaw(w, Reliability::Reliable, kHostPlayer);
    return SendRaw(hostPeer_, w, Reliability::Reliable);
}

// ---------------------------------------------------------------------------------------------------------------
// helpers

bool Session::SendRaw(PeerId peer, const ByteWriter& w, Reliability rel, uint8_t lane) {
    if (!peer) return false;
    if (lane && rel == Reliability::Reliable) return transport_.SendOnLane(peer, w.Span(), rel, lane);
    return transport_.Send(peer, w.Span(), rel);
}

bool Session::BroadcastRaw(const ByteWriter& w, Reliability rel, PlayerId except, uint8_t lane) {
    lastCopies_ = lastFailed_ = 0;
    size_t limit = rel == Reliability::Reliable ? kMaxReliableMessage : kMaxUnreliableMessage;
    bool ok = true;
    for (auto& p : players_) {
        if (p.id == kHostPlayer || p.id == except) continue;
        ++lastCopies_;
        const bool sent = w.Size() <= limit && SendRaw(p.peer, w, rel, lane);
        if (!sent) ++lastFailed_;
        ok &= sent;
    }
    return ok;
}

const PlayerInfo* Session::FindPlayer(PlayerId id) const {
    for (auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

PlayerInfo* Session::FindMutable(PlayerId id) {
    for (auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

PlayerInfo* Session::FindByPeer(PeerId peer) {
    for (auto& p : players_)
        if (p.peer == peer && p.id != kHostPlayer) return &p;
    return nullptr;
}

}  // namespace mc::net
