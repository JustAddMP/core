// net: co-op session over any Transport. Host-authoritative star: guests talk only to the host,
// the host relays. Handshake, player list, ping, chat and generic app messages. See docs/protocol.md.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "net/transport.h"
#include "net/wire.h"

namespace mc::net {

// Bump when the session or any app message format changes incompatibly.
constexpr uint32_t kProtocolVersion = 5;  // 5: optional session password (Challenge/Auth)

using PlayerId = uint8_t;
constexpr PlayerId kHostPlayer = 0;
constexpr PlayerId kNoPlayer = 0xFE;
constexpr PlayerId kAllPlayers = 0xFF;
constexpr int kMaxPlayers = 4;

struct PlayerInfo {
    PlayerId id = kNoPlayer;
    std::string name;
    std::string model;           // character the player appears as (spawn profile; "" = default for his id)
    PeerId peer = kInvalidPeer;  // host: transport peer of that guest; guest: host's peer for id 0, else 0
    float rttMs = 0;             // to the host (host's own entry: 0)
};

struct SessionConfig {
    uint32_t protocolVersion = kProtocolVersion;
    uint32_t modBuild = 0;
    std::string playerName = "Player";
    std::string playerModel;  // spawn profile we appear as, sent in Hello ("" = default by player id)
    std::string modName = "mod";  // for messages ("Both players need the same <modName> version.")
    int maxPlayers = kMaxPlayers;
    double pingIntervalS = 1.0;
    double helloTimeoutS = 5.0;   // host: drop a connection that has not sent Hello
    double welcomeTimeoutS = 10.0; // guest: give up if no Welcome/Reject

    // Session password (docs/protocol.md "Session password"). Host: required from guests ("" = open session).
    // Guest: answers the host's challenge with it ("" = none; the host then rejects with "password required").
    std::string password;
    // Host: guests for which isInvitedPeer() is true (Steam lobby members) skip the password.
    bool invitesSkipPassword = true;
    int authMaxFailures = 5;       // host: wrong answers per address within authWindowS before it locks out
    double authWindowS = 60.0;
    double authFailDelayS = 1.0;   // host: a wrong answer is rejected only after this long

    // Host: per-guest rate limits (token buckets; docs/protocol.md "Rate limits"). Chat and app messages the host
    // relays to other guests (target not the host) above these are dropped (not handled, not relayed) and counted
    // (Session::RateLimits()). A rate <= 0 turns that limit off.
    double chatPerS = 5.0;                   // chat messages per second
    double chatBurst = 10.0;                 // chat messages at once
    double relayBytesPerS = 64.0 * 1024;     // relayed app payload bytes per second
    double relayBurstBytes = 128.0 * 1024;   // relayed app payload bytes at once (one larger message still passes
                                             // when the bucket is full; it then has to refill)
};

// Host: what the rate limits dropped, summed over all guests since Host().
struct RateLimitStats {
    uint64_t chatDropped = 0;
    uint64_t appDropped = 0;
    uint64_t appBytesDropped = 0;
};

// Why the host rejected us (guest), from the Reject code. Values are the wire codes.
enum class RejectReason : uint8_t {
    None = 0,
    Version = 1,
    Full = 2,
    BadHello = 3,
    PasswordRequired = 4,
    WrongPassword = 5,
    TooManyAttempts = 6,
};

enum class SessionState { Idle, Hosting, Connecting, Connected, Disconnected };

class Session {
public:
    // from: sender's player id. payload: app message body (after the header).
    using AppHandler = std::function<void(PlayerId from, ByteReader& payload)>;

    Session(Transport& transport, SessionConfig cfg);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Host(uint16_t port);
    bool Join(std::string_view address);
    void Leave();
    void Poll();  // once per frame

    // ---- app messages (ids are free for later phases; see docs/protocol.md for the allocation) ----
    void On(uint16_t msgId, AppHandler handler);
    void Off(uint16_t msgId);
    // Guest: to the host only. Host: handled locally (loopback) for symmetric code paths.
    bool SendToHost(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel = Reliability::Reliable);
    // Host: to every guest. Guest: to the host, which handles it and relays it to the other guests.
    bool Broadcast(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel = Reliability::Reliable);
    // To one player. Guest->guest goes through the host relay.
    bool SendTo(PlayerId player, uint16_t msgId, std::span<const uint8_t> payload,
                Reliability rel = Reliability::Reliable);
    bool SendChat(std::string_view text);

    // ---- state ----
    SessionState State() const { return state_; }
    bool IsHost() const { return state_ == SessionState::Hosting; }
    bool IsActive() const { return state_ == SessionState::Hosting || state_ == SessionState::Connected; }
    PlayerId LocalId() const { return localId_; }
    const std::vector<PlayerInfo>& Players() const { return players_; }
    const PlayerInfo* FindPlayer(PlayerId id) const;
    const std::string& LastError() const { return lastError_; }
    Transport& GetTransport() { return transport_; }
    const SessionConfig& Config() const { return cfg_; }
    const RateLimitStats& RateLimits() const { return rateStats_; }

    // ---- session password ----
    // Host: change the required password at any time ("" = none; players already in stay). Guest: the password
    // for the next Join / RetryWithPassword.
    void SetPassword(std::string password) { cfg_.password = std::move(password); }
    bool HasPassword() const { return !cfg_.password.empty(); }
    // Guest: why the last join was rejected (None while connecting/connected or after a non-reject failure).
    RejectReason LastReject() const { return lastReject_; }
    // Guest: the host wants a (different) password: prompt for one and call RetryWithPassword.
    bool NeedsPassword() const {
        return lastReject_ == RejectReason::PasswordRequired || lastReject_ == RejectReason::WrongPassword;
    }
    // Guest: join the last address again with this password. False if there was no earlier Join.
    bool RetryWithPassword(std::string password);
    // Host: peers that count as invited (Steam lobby members). With invitesSkipPassword they are not challenged.
    std::function<bool(PeerId)> isInvitedPeer;

    // ---- events ----
    std::function<void()> onJoined;                           // guest: Welcome received
    std::function<void(const PlayerInfo&)> onPlayerJoined;    // someone else joined
    std::function<void(const PlayerInfo&, const std::string& reason)> onPlayerLeft;
    std::function<void(PlayerId from, const std::string& text)> onChat;
    std::function<void(const std::string& reason)> onDisconnected;  // guest: lost/rejected/kicked; host: never

private:
    struct Pending {  // host: connected transport peer that has not completed the handshake
        double since = 0;
        double kickAt = 0;  // >0: rejected, drop at this time (lets the Reject be delivered)
        // password: Hello accepted, Challenge sent, waiting for Auth
        bool awaitingAuth = false;
        std::array<uint8_t, 32> challenge{};
        std::string name, model;  // from the Hello, used once Auth succeeds
        // delayed Reject (wrong password): sent at rejectAt
        double rejectAt = 0;
        uint8_t rejectCode = 0;
        std::string rejectReason;
    };

    struct Bucket {  // token bucket
        bool started = false;  // starts full
        double tokens = 0;     // may go below 0 after one message larger than what was left
        double last = 0;
        // Takes cost if anything is left (a cost above the burst passes on a full bucket, then refills).
        bool Take(double cost, double rate, double burst, double now);
    };
    struct GuestLimits {
        Bucket chat, relay;
        // dropped since the last rate-limit log line
        uint32_t chatDropped = 0, appDropped = 0;
        uint64_t appBytesDropped = 0;
    };
    void CountDropped(PlayerInfo& from, bool chat, size_t bytes);
    void LogRateLimits(double now);

    void OnEvent(const Event& e);
    void HostOnMessage(PeerId peer, ByteReader& r, Reliability ch);
    void GuestOnMessage(ByteReader& r);
    void HostOnHello(PeerId peer, ByteReader& r);
    void HostOnAuth(PeerId peer, ByteReader& r);
    void HostAdmit(PeerId peer, const std::string& name, const std::string& model);
    std::string AttemptKey(PeerId peer) const;
    bool LockedOut(const std::string& key, double now);
    void Reject(PeerId peer, uint8_t code, const std::string& reason);
    void HostRemovePlayer(PeerId peer, const std::string& reason);
    void DispatchApp(uint16_t id, PlayerId from, std::span<const uint8_t> payload);
    bool SendRaw(PeerId peer, const ByteWriter& w, Reliability rel);
    bool BroadcastRaw(const ByteWriter& w, Reliability rel, PlayerId except);  // false if any send failed
    void Fail(const std::string& reason);
    void Reset();
    PlayerInfo* FindByPeer(PeerId peer);
    PlayerInfo* FindMutable(PlayerId id);
    ByteWriter AppHeader(uint16_t id, PlayerId from, PlayerId target, Reliability rel) const;

    Transport& transport_;
    SessionConfig cfg_;
    SessionState state_ = SessionState::Idle;
    PlayerId localId_ = kNoPlayer;
    PeerId hostPeer_ = kInvalidPeer;
    std::vector<PlayerInfo> players_;
    std::unordered_map<PeerId, Pending> pending_;
    std::unordered_map<uint16_t, AppHandler> handlers_;
    std::string lastError_;
    std::string lastJoinAddress_;
    RejectReason lastReject_ = RejectReason::None;
    std::unordered_map<std::string, std::vector<double>> authFailures_;  // host: attempt key -> failure times
    double helloSentAt_ = 0, lastPing_ = 0, lastPingTable_ = 0;
    std::unordered_map<PlayerId, GuestLimits> limits_;  // host: per guest
    RateLimitStats rateStats_;
    double lastRateLog_ = -1e9;
    bool rateLogPending_ = false;
    bool welcomed_ = false;
};

}  // namespace mc::net
