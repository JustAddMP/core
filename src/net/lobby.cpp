#include "net/lobby.h"

#include "net/wire.h"

namespace mc::net {
namespace {
constexpr uint8_t kStateVersion = 1;
constexpr uint8_t kNoMode = 0xFF;

uint8_t ModeByte(int mode) { return mode < 0 || mode > 254 ? kNoMode : static_cast<uint8_t>(mode); }
int ModeInt(uint8_t b) { return b == kNoMode ? -1 : b; }
}  // namespace

void Lobby::Attach(Session& session) {
    Detach();
    session_ = &session;
    ready_.clear();
    mode_ = 0;
    inGame_ = false;
    gameMode_ = -1;
    startSeq_ = 0;
    dirty_ = true;
    localReady_ = false;
    lastPlayers_ = 0;
    lastPlayerMask_ = 0;
    session.On(kMsgState, [this](PlayerId from, ByteReader& r) { OnState(from, r); });
    session.On(kMsgReady, [this](PlayerId from, ByteReader& r) { OnReady(from, r); });
    session.On(kMsgStart, [this](PlayerId from, ByteReader& r) { OnStart(from, r); });
}

void Lobby::Detach() {
    if (!session_) return;
    session_->Off(kMsgState);
    session_->Off(kMsgReady);
    session_->Off(kMsgStart);
    session_ = nullptr;
    ready_.clear();
    inGame_ = false;
    gameMode_ = -1;
}

void Lobby::Changed() {
    if (onChanged) onChanged();
}

void Lobby::Tick() {
    if (!session_ || !session_->IsHost()) return;
    // Player set changed: newcomers start not ready, leavers are forgotten, and everyone gets the state again (a
    // newcomer has none yet).
    // (joinSeq: a player who lost his connection and came back counts as new, even within one Poll.)
    uint32_t mask = 0;
    size_t seqs = 0;
    for (const auto& p : session_->Players()) {
        mask |= 1u << (p.id & 31);
        seqs += p.joinSeq;
    }
    if (mask != lastPlayerMask_ || seqs != lastPlayers_) {
        lastPlayerMask_ = mask;
        lastPlayers_ = seqs;
        std::map<PlayerId, bool> next;
        for (const auto& p : session_->Players()) {
            auto it = ready_.find(p.id);
            next[p.id] = p.id != kHostPlayer && it != ready_.end() && it->second;
        }
        ready_.swap(next);
        dirty_ = true;
        Changed();
    }
    if (dirty_) SendState();
}

void Lobby::SendState() {
    dirty_ = false;
    if (!session_ || !session_->IsHost()) return;
    ByteWriter w(32);
    w.U8(kStateVersion);
    w.U8(ModeByte(mode_));
    w.U8(inGame_ ? 1 : 0);
    w.U8(ModeByte(gameMode_));
    w.U32(startSeq_);
    w.U8(static_cast<uint8_t>(ready_.size()));
    for (const auto& [id, ready] : ready_) {
        w.U8(id);
        w.U8(ready ? 1 : 0);
    }
    session_->Broadcast(kMsgState, w.Span(), Reliability::Reliable);
}

void Lobby::SetMode(int mode) {
    if (!session_ || !session_->IsHost() || mode < 0 || mode > 254 || mode == mode_) return;
    mode_ = mode;
    dirty_ = true;
    Changed();
}

void Lobby::SetInGame(bool inGame, int mode) {
    if (!session_ || !session_->IsHost()) return;
    if (!inGame) mode = -1;
    if (inGame == inGame_ && mode == gameMode_) return;
    inGame_ = inGame;
    gameMode_ = mode;
    dirty_ = true;
    Changed();
}

bool Lobby::AllReady() const {
    for (const auto& [id, ready] : ready_)
        if (id != kHostPlayer && !ready) return false;
    return true;
}

bool Lobby::Start(int mode, bool force) {
    if (!session_ || !session_->IsHost() || mode < 0 || mode > 254) return false;
    if (!force && !AllReady()) return false;
    ++startSeq_;
    mode_ = mode;
    ByteWriter w(8);
    w.U8(ModeByte(mode));
    w.U32(startSeq_);
    session_->Broadcast(kMsgStart, w.Span(), Reliability::Reliable);
    // Everyone readies up again for the next start.
    for (auto& [id, ready] : ready_) ready = false;
    dirty_ = true;
    Changed();
    if (onStart) onStart(mode);
    return true;
}

void Lobby::SetReady(bool ready) {
    if (!session_ || session_->IsHost() || !session_->IsActive()) return;
    localReady_ = ready;
    ready_[session_->LocalId()] = ready;
    ByteWriter w(2);
    w.U8(ready ? 1 : 0);
    session_->SendToHost(kMsgReady, w.Span(), Reliability::Reliable);
    Changed();
}

bool Lobby::Ready(PlayerId id) const {
    auto it = ready_.find(id);
    return it != ready_.end() && it->second;
}

void Lobby::OnState(PlayerId from, ByteReader& r) {
    if (!session_ || session_->IsHost() || from != kHostPlayer) return;
    const uint8_t version = r.U8();
    const uint8_t mode = r.U8();
    const uint8_t inGame = r.U8();
    const uint8_t gameMode = r.U8();
    const uint32_t seq = r.U32();
    const uint8_t n = r.U8();
    if (!r.Ok() || version != kStateVersion || n > kMaxPlayers) return;
    std::map<PlayerId, bool> next;
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t id = r.U8();
        const uint8_t ready = r.U8();
        if (!r.Ok() || id >= kMaxPlayers) return;
        next[id] = ready != 0;
    }
    ready_.swap(next);
    mode_ = ModeInt(mode);
    inGame_ = inGame != 0;
    gameMode_ = ModeInt(gameMode);
    if (seq > startSeq_) startSeq_ = seq;
    Changed();
}

void Lobby::OnReady(PlayerId from, ByteReader& r) {
    if (!session_ || !session_->IsHost() || from == kHostPlayer) return;
    const uint8_t ready = r.U8();
    if (!r.Ok() || !session_->FindPlayer(from)) return;
    if (ready_[from] == (ready != 0)) return;
    ready_[from] = ready != 0;
    dirty_ = true;
    Changed();
}

void Lobby::OnStart(PlayerId from, ByteReader& r) {
    if (!session_ || session_->IsHost() || from != kHostPlayer) return;
    const uint8_t mode = r.U8();
    const uint32_t seq = r.U32();
    if (!r.Ok() || mode == kNoMode || seq <= startSeq_) return;
    startSeq_ = seq;
    mode_ = mode;
    localReady_ = false;
    for (auto& [id, ready] : ready_) ready = false;
    Changed();
    if (onStart) onStart(mode);
}

}  // namespace mc::net
