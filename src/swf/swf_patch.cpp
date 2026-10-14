#include "game/swf_patch.h"

#include "core/log.h"

#include <cstring>

namespace mc::game::swf {
namespace {

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t i = 0;
    bool ok = true;

    bool Need(size_t k) {
        if (!ok || i + k > n) {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t U8() { return Need(1) ? p[i++] : 0; }
    uint16_t U16() {
        if (!Need(2)) return 0;
        uint16_t v = static_cast<uint16_t>(p[i] | (p[i + 1] << 8));
        i += 2;
        return v;
    }
    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, p + i, 4);
        i += 4;
        return v;
    }
    uint32_t U30() {
        uint32_t r = 0;
        for (int k = 0; k < 5; ++k) {
            if (!Need(1)) return 0;
            const uint8_t b = p[i++];
            r |= static_cast<uint32_t>(b & 0x7F) << (7 * k);
            if (!(b & 0x80)) break;
        }
        return r;
    }
    void Skip(size_t k) {
        if (Need(k)) i += k;
    }
};

void PutU30(std::vector<uint8_t>& o, uint32_t v) {
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        o.push_back(b);
    } while (v);
}
void PutU16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
void PutU32(std::vector<uint8_t>& o, uint32_t v) {
    for (int k = 0; k < 4; ++k) o.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
uint32_t Count(size_t vectorSize) { return vectorSize <= 1 ? 0 : static_cast<uint32_t>(vectorSize); }

}  // namespace

// ---- Abc ---------------------------------------------------------------------------------------------------------
bool Abc::Parse(const uint8_t* data, size_t size, std::string* err) {
    Reader r{data, size};
    auto fail = [&](const char* what) {
        if (err) *err = std::string("abc: ") + what + " at " + std::to_string(r.i);
        return false;
    };
    _minor = r.U16();
    _major = r.U16();
    if (_major != 46) return fail("unsupported version");

    // ints / uints / doubles: kept raw (counts included)
    size_t s = r.i;
    uint32_t c = r.U30();
    for (uint32_t k = 1; k < c && r.ok; ++k) r.U30();
    _ints.assign(data + s, data + r.i);
    s = r.i;
    c = r.U30();
    for (uint32_t k = 1; k < c && r.ok; ++k) r.U30();
    _uints.assign(data + s, data + r.i);
    s = r.i;
    c = r.U30();
    if (c > 1) r.Skip(static_cast<size_t>(c - 1) * 8);
    _doubles.assign(data + s, data + r.i);

    c = r.U30();
    _strings.assign(1, std::string());
    for (uint32_t k = 1; k < c && r.ok; ++k) {
        const uint32_t len = r.U30();
        if (!r.Need(len)) break;
        _strings.emplace_back(reinterpret_cast<const char*>(data + r.i), len);
        r.i += len;
    }
    c = r.U30();
    _namespaces.assign(1, Ns{0, 0});
    for (uint32_t k = 1; k < c && r.ok; ++k) {
        const uint8_t kind = r.U8();
        _namespaces.push_back({kind, r.U30()});
    }
    c = r.U30();
    _nsSets.assign(1, {});
    for (uint32_t k = 1; k < c && r.ok; ++k) {
        const uint32_t n = r.U30();
        std::vector<uint32_t> set;
        for (uint32_t j = 0; j < n && r.ok; ++j) set.push_back(r.U30());
        _nsSets.push_back(std::move(set));
    }
    c = r.U30();
    _multinames.assign(1, Multiname{0});
    for (uint32_t k = 1; k < c && r.ok; ++k) {
        Multiname m{r.U8()};
        switch (m.kind) {
        case 0x07: case 0x0D: m.a = r.U30(); m.b = r.U30(); break;            // QName(A)
        case 0x0F: case 0x10: m.a = r.U30(); break;                           // RTQName(A)
        case 0x11: case 0x12: break;                                          // RTQNameL(A)
        case 0x09: case 0x0E: m.a = r.U30(); m.b = r.U30(); break;            // Multiname(A): name, nsset
        case 0x1B: case 0x1C: m.a = r.U30(); break;                           // MultinameL(A): nsset
        case 0x1D: {                                                          // TypeName
            m.a = r.U30();
            const uint32_t n = r.U30();
            for (uint32_t j = 0; j < n && r.ok; ++j) m.params.push_back(r.U30());
            break;
        }
        default: return fail("unknown multiname kind");
        }
        _multinames.push_back(std::move(m));
    }
    if (!r.ok) return fail("truncated constant pool");

    // ---- methods .. scripts: kept verbatim, but walked to find traits and the start of the bodies ----
    const size_t middleStart = r.i;
    auto readTraits = [&](std::vector<Trait>* out) {
        const uint32_t n = r.U30();
        for (uint32_t k = 0; k < n && r.ok; ++k) {
            Trait t{r.U30(), 0, 0};
            const uint8_t kind = r.U8();
            t.kind = kind & 0x0F;
            switch (t.kind) {
            case 0: case 6: {  // slot / const
                r.U30();
                r.U30();
                if (r.U30()) r.U8();
                break;
            }
            case 1: case 2: case 3: r.U30(); t.index = r.U30(); break;  // method/getter/setter
            case 4: case 5: r.U30(); t.index = r.U30(); break;          // class / function
            default: r.ok = false; break;
            }
            if ((kind >> 4) & 0x04) {
                const uint32_t m = r.U30();
                for (uint32_t j = 0; j < m && r.ok; ++j) r.U30();
            }
            if (out) out->push_back(t);
        }
    };
    c = r.U30();  // methods
    for (uint32_t k = 0; k < c && r.ok; ++k) {
        const uint32_t pc = r.U30();
        r.U30();  // return type
        for (uint32_t j = 0; j < pc && r.ok; ++j) r.U30();
        r.U30();  // name
        const uint8_t flags = r.U8();
        if (flags & 0x08) {
            const uint32_t oc = r.U30();
            for (uint32_t j = 0; j < oc && r.ok; ++j) {
                r.U30();
                r.U8();
            }
        }
        if (flags & 0x80)
            for (uint32_t j = 0; j < pc && r.ok; ++j) r.U30();
    }
    c = r.U30();  // metadata
    for (uint32_t k = 0; k < c && r.ok; ++k) {
        r.U30();
        const uint32_t n = r.U30();
        for (uint32_t j = 0; j < n && r.ok; ++j) {
            r.U30();
            r.U30();
        }
    }
    const uint32_t classes = r.U30();
    _instTraits.assign(classes, {});
    _classTraits.assign(classes, {});
    _instNames.assign(classes, 0);
    for (uint32_t k = 0; k < classes && r.ok; ++k) {
        _instNames[k] = r.U30();
        r.U30();  // super
        const uint8_t flags = r.U8();
        if (flags & 0x08) r.U30();
        const uint32_t ni = r.U30();
        for (uint32_t j = 0; j < ni && r.ok; ++j) r.U30();
        r.U30();  // iinit
        readTraits(&_instTraits[k]);
    }
    for (uint32_t k = 0; k < classes && r.ok; ++k) {
        r.U30();  // cinit
        readTraits(&_classTraits[k]);
    }
    c = r.U30();  // scripts
    for (uint32_t k = 0; k < c && r.ok; ++k) {
        r.U30();
        readTraits(nullptr);
    }
    if (!r.ok) return fail("truncated class/script section");
    _middle.assign(data + middleStart, data + r.i);

    c = r.U30();  // method bodies
    _bodies.clear();
    for (uint32_t k = 0; k < c && r.ok; ++k) {
        Body b{};
        b.method = r.U30();
        b.maxStack = r.U30();
        b.localCount = r.U30();
        b.initScope = r.U30();
        b.maxScope = r.U30();
        const uint32_t len = r.U30();
        if (!r.Need(len)) break;
        b.code.assign(data + r.i, data + r.i + len);
        r.i += len;
        const uint32_t ne = r.U30();
        for (uint32_t j = 0; j < ne && r.ok; ++j) {
            Body::Exc e{};
            e.from = r.U30();
            e.to = r.U30();
            e.target = r.U30();
            e.type = r.U30();
            e.name = r.U30();
            b.exceptions.push_back(e);
        }
        const size_t ts = r.i;
        readTraits(nullptr);
        b.traits.assign(data + ts, data + r.i);
        _bodies.push_back(std::move(b));
    }
    if (!r.ok) return fail("truncated method bodies");
    if (r.i != size) MC_LOG_WARN("swf: abc has %zu trailing bytes (kept out)", size - r.i);
    return true;
}

std::vector<uint8_t> Abc::Write() const {
    std::vector<uint8_t> o;
    o.reserve(_middle.size() + 65536);
    PutU16(o, _minor);
    PutU16(o, _major);
    o.insert(o.end(), _ints.begin(), _ints.end());
    o.insert(o.end(), _uints.begin(), _uints.end());
    o.insert(o.end(), _doubles.begin(), _doubles.end());
    PutU30(o, Count(_strings.size()));
    for (size_t k = 1; k < _strings.size(); ++k) {
        PutU30(o, static_cast<uint32_t>(_strings[k].size()));
        o.insert(o.end(), _strings[k].begin(), _strings[k].end());
    }
    PutU30(o, Count(_namespaces.size()));
    for (size_t k = 1; k < _namespaces.size(); ++k) {
        o.push_back(_namespaces[k].kind);
        PutU30(o, _namespaces[k].name);
    }
    PutU30(o, Count(_nsSets.size()));
    for (size_t k = 1; k < _nsSets.size(); ++k) {
        PutU30(o, static_cast<uint32_t>(_nsSets[k].size()));
        for (uint32_t v : _nsSets[k]) PutU30(o, v);
    }
    PutU30(o, Count(_multinames.size()));
    for (size_t k = 1; k < _multinames.size(); ++k) {
        const Multiname& m = _multinames[k];
        o.push_back(m.kind);
        switch (m.kind) {
        case 0x07: case 0x0D: case 0x09: case 0x0E: PutU30(o, m.a); PutU30(o, m.b); break;
        case 0x0F: case 0x10: case 0x1B: case 0x1C: PutU30(o, m.a); break;
        case 0x1D:
            PutU30(o, m.a);
            PutU30(o, static_cast<uint32_t>(m.params.size()));
            for (uint32_t v : m.params) PutU30(o, v);
            break;
        default: break;
        }
    }
    o.insert(o.end(), _middle.begin(), _middle.end());
    PutU30(o, static_cast<uint32_t>(_bodies.size()));
    for (const Body& b : _bodies) {
        PutU30(o, b.method);
        PutU30(o, b.maxStack);
        PutU30(o, b.localCount);
        PutU30(o, b.initScope);
        PutU30(o, b.maxScope);
        PutU30(o, static_cast<uint32_t>(b.code.size()));
        o.insert(o.end(), b.code.begin(), b.code.end());
        PutU30(o, static_cast<uint32_t>(b.exceptions.size()));
        for (const auto& e : b.exceptions) {
            PutU30(o, e.from);
            PutU30(o, e.to);
            PutU30(o, e.target);
            PutU30(o, e.type);
            PutU30(o, e.name);
        }
        o.insert(o.end(), b.traits.begin(), b.traits.end());
    }
    return o;
}

uint32_t Abc::String(std::string_view s) {
    for (size_t k = 1; k < _strings.size(); ++k)
        if (_strings[k] == s) return static_cast<uint32_t>(k);
    _strings.emplace_back(s);
    return static_cast<uint32_t>(_strings.size() - 1);
}

uint32_t Abc::Namespace(uint8_t kind, std::string_view name) {
    for (size_t k = 1; k < _namespaces.size(); ++k) {
        const Ns& n = _namespaces[k];
        if (n.kind == kind && n.name < _strings.size() && _strings[n.name] == name) return static_cast<uint32_t>(k);
    }
    const uint32_t s = String(name);
    _namespaces.push_back({kind, s});
    return static_cast<uint32_t>(_namespaces.size() - 1);
}

uint32_t Abc::QName(uint8_t nsKind, std::string_view nsName, std::string_view name) {
    for (size_t k = 1; k < _multinames.size(); ++k) {
        const Multiname& m = _multinames[k];
        if (m.kind != 0x07 || m.a >= _namespaces.size() || m.b >= _strings.size()) continue;
        const Ns& n = _namespaces[m.a];
        if (n.kind == nsKind && n.name < _strings.size() && _strings[n.name] == nsName && _strings[m.b] == name)
            return static_cast<uint32_t>(k);
    }
    Multiname m{0x07};
    m.a = Namespace(nsKind, nsName);
    m.b = String(name);
    _multinames.push_back(m);
    return static_cast<uint32_t>(_multinames.size() - 1);
}

uint32_t Abc::RuntimeIndexName() {
    for (size_t k = 1; k < _multinames.size(); ++k) {
        const Multiname& m = _multinames[k];
        if (m.kind != 0x1B || m.a >= _nsSets.size()) continue;
        for (uint32_t nsIdx : _nsSets[m.a]) {
            if (nsIdx < _namespaces.size() && _namespaces[nsIdx].kind == ns::kPackage &&
                _namespaces[nsIdx].name < _strings.size() && _strings[_namespaces[nsIdx].name].empty())
                return static_cast<uint32_t>(k);
        }
    }
    _nsSets.push_back({Namespace(ns::kPackage, "")});
    Multiname m{0x1B};
    m.a = static_cast<uint32_t>(_nsSets.size() - 1);
    _multinames.push_back(m);
    return static_cast<uint32_t>(_multinames.size() - 1);
}

uint32_t Abc::Name(std::string_view name) {
    for (size_t k = 1; k < _multinames.size(); ++k) {
        const Multiname& m = _multinames[k];
        if (m.kind == 0x09 && m.a < _strings.size() && _strings[m.a] == name) return static_cast<uint32_t>(k);
    }
    return Public(name);
}

std::string Abc::NameOf(uint32_t mn) const {
    if (mn == 0 || mn >= _multinames.size()) return "*";
    const Multiname& m = _multinames[mn];
    if ((m.kind == 0x07 || m.kind == 0x0D) && m.b < _strings.size()) {
        const std::string nsName =
            m.a < _namespaces.size() && _namespaces[m.a].name < _strings.size() ? _strings[_namespaces[m.a].name] : "";
        return nsName.empty() ? _strings[m.b] : nsName + "::" + _strings[m.b];
    }
    if ((m.kind == 0x09 || m.kind == 0x0E || m.kind == 0x0F || m.kind == 0x10) && m.a < _strings.size())
        return _strings[m.a];
    return "[rt]";
}

int Abc::FindClass(std::string_view nsName, std::string_view name) const {
    for (size_t k = 0; k < _instNames.size(); ++k) {
        const uint32_t mn = _instNames[k];
        if (mn == 0 || mn >= _multinames.size()) continue;
        const Multiname& m = _multinames[mn];
        if (m.kind != 0x07 || m.a >= _namespaces.size() || m.b >= _strings.size()) continue;
        const uint32_t nsStr = _namespaces[m.a].name;
        if (nsStr < _strings.size() && _strings[nsStr] == nsName && _strings[m.b] == name) return static_cast<int>(k);
    }
    return -1;
}

uint32_t Abc::TraitName(int cls, std::string_view name) const {
    if (cls < 0 || static_cast<size_t>(cls) >= _instTraits.size()) return 0;
    for (const auto* list : {&_instTraits[cls], &_classTraits[cls]}) {
        for (const Trait& t : *list) {
            if (t.name >= _multinames.size()) continue;
            const Multiname& m = _multinames[t.name];
            if (m.kind == 0x07 && m.b < _strings.size() && _strings[m.b] == name) return t.name;
        }
    }
    return 0;
}

int Abc::TraitMethod(int cls, std::string_view name) const {
    if (cls < 0 || static_cast<size_t>(cls) >= _instTraits.size()) return -1;
    for (const Trait& t : _instTraits[cls]) {
        if (t.kind != 1 || t.name >= _multinames.size()) continue;
        const Multiname& m = _multinames[t.name];
        if (m.kind == 0x07 && m.b < _strings.size() && _strings[m.b] == name) return static_cast<int>(t.index);
    }
    return -1;
}

uint32_t Abc::LocalCount(int method) const {
    for (const Body& b : _bodies)
        if (b.method == static_cast<uint32_t>(method)) return b.localCount;
    return 0;
}

bool Abc::Prepend(int method, const std::vector<uint8_t>& code, uint32_t extraStack, uint32_t extraLocals) {
    for (Body& b : _bodies) {
        if (b.method != static_cast<uint32_t>(method)) continue;
        b.code.insert(b.code.begin(), code.begin(), code.end());
        const uint32_t shift = static_cast<uint32_t>(code.size());
        for (auto& e : b.exceptions) {
            e.from += shift;
            e.to += shift;
            e.target += shift;
        }
        b.maxStack += extraStack;
        b.localCount += extraLocals;
        if (b.maxScope < b.initScope + 1) b.maxScope = b.initScope + 1;  // our prologue pushes 'this'
        return true;
    }
    return false;
}

// ---- Asm ---------------------------------------------------------------------------------------------------------
Asm& Asm::Op(uint8_t o) {
    _code.push_back(o);
    return *this;
}
Asm& Asm::U30(uint32_t v) {
    PutU30(_code, v);
    return *this;
}
Asm& Asm::GetLocal(uint32_t r) {
    if (r <= 3) return Op(static_cast<uint8_t>(0xD0 + r));
    return Op(0x62).U30(r);
}
Asm& Asm::SetLocal(uint32_t r) {
    if (r <= 3) return Op(static_cast<uint8_t>(0xD4 + r));
    return Op(0x63).U30(r);
}
Asm& Asm::PushString(std::string_view s) { return Op(0x2C).U30(_abc.String(s)); }
Asm& Asm::PushInt(int v) {
    if (v >= -128 && v <= 127) {
        Op(0x24);
        return Op(static_cast<uint8_t>(static_cast<int8_t>(v)));
    }
    return Op(0x25).U30(static_cast<uint32_t>(v));  // pushshort (u30, sign-extended from 16 bits)
}
Asm& Asm::GetProperty(uint32_t mn) { return Op(0x66).U30(mn); }
Asm& Asm::SetProperty(uint32_t mn) { return Op(0x61).U30(mn); }
Asm& Asm::GetLex(uint32_t mn) { return Op(0x60).U30(mn); }
Asm& Asm::FindPropStrict(uint32_t mn) { return Op(0x5D).U30(mn); }
Asm& Asm::CallProp(uint32_t mn, uint32_t argc) { return Op(0x46).U30(mn).U30(argc); }
Asm& Asm::CallPropVoid(uint32_t mn, uint32_t argc) { return Op(0x4F).U30(mn).U30(argc); }
Asm& Asm::ConstructProp(uint32_t mn, uint32_t argc) { return Op(0x4A).U30(mn).U30(argc); }

int Asm::Label() {
    _labels.push_back(-1);
    return static_cast<int>(_labels.size() - 1);
}
Asm& Asm::Bind(int label) {
    _labels[static_cast<size_t>(label)] = static_cast<long>(_code.size());
    return *this;
}
Asm& Asm::LoopHead(int label) {
    Bind(label);
    return Op(op::kLabel);
}
Asm& Asm::Jump(uint8_t branchOp, int label) {
    Op(branchOp);
    _fixes.push_back({_code.size(), label});
    _code.insert(_code.end(), 3, 0);
    return *this;
}

std::vector<uint8_t> Asm::Finish() {
    for (const Fix& f : _fixes) {
        const long target = _labels[static_cast<size_t>(f.label)];
        if (target < 0) {
            MC_LOG_ERROR("swf: asm: unbound label %d", f.label);
            return {};
        }
        const long rel = target - static_cast<long>(f.at + 3);
        _code[f.at] = static_cast<uint8_t>(rel & 0xFF);
        _code[f.at + 1] = static_cast<uint8_t>((rel >> 8) & 0xFF);
        _code[f.at + 2] = static_cast<uint8_t>((rel >> 16) & 0xFF);
    }
    return _code;
}

// ---- movie container -----------------------------------------------------------------------------------------------
bool PatchMovie(std::vector<uint8_t>& movie, const std::function<bool(Abc&, std::string*)>& patch, std::string* err) {
    auto fail = [&](const std::string& what) {
        if (err) *err = what;
        return false;
    };
    if (movie.size() < 16) return fail("file too small");
    const bool plain = (movie[0] == 'F' && movie[1] == 'W' && movie[2] == 'S') ||
                       (movie[0] == 'G' && movie[1] == 'F' && movie[2] == 'X');
    if (!plain) return fail("not an uncompressed SWF/GFX movie");
    const int nbits = movie[8] >> 3;
    const size_t rectBytes = static_cast<size_t>((5 + 4 * nbits + 7) / 8);
    size_t pos = 8 + rectBytes + 4;
    if (pos > movie.size()) return fail("bad header");

    std::vector<uint8_t> out(movie.begin(), movie.begin() + static_cast<long>(pos));
    int patched = 0;
    while (pos + 2 <= movie.size()) {
        const size_t start = pos;
        const uint16_t cl = static_cast<uint16_t>(movie[pos] | (movie[pos + 1] << 8));
        pos += 2;
        const uint16_t code = cl >> 6;
        uint32_t len = cl & 0x3F;
        if (len == 0x3F) {
            if (pos + 4 > movie.size()) return fail("truncated tag header");
            std::memcpy(&len, movie.data() + pos, 4);
            pos += 4;
        }
        if (pos + len > movie.size()) return fail("truncated tag");
        if (code == 82 || code == 72) {
            size_t abcStart = pos;
            if (code == 82) {
                abcStart += 4;
                while (abcStart < pos + len && movie[abcStart] != 0) ++abcStart;
                ++abcStart;  // name terminator
                if (abcStart > pos + len) return fail("bad DoABC tag");  // flags/name run past the tag
            }
            Abc abc;
            std::string why;
            if (!abc.Parse(movie.data() + abcStart, pos + len - abcStart, &why)) return fail(why);
            if (!patch(abc, &why)) return fail(why);
            const std::vector<uint8_t> newAbc = abc.Write();
            const uint32_t newLen = static_cast<uint32_t>((abcStart - pos) + newAbc.size());
            PutU16(out, static_cast<uint16_t>((code << 6) | 0x3F));
            PutU32(out, newLen);
            out.insert(out.end(), movie.begin() + static_cast<long>(pos), movie.begin() + static_cast<long>(abcStart));
            out.insert(out.end(), newAbc.begin(), newAbc.end());
            ++patched;
        } else {
            out.insert(out.end(), movie.begin() + static_cast<long>(start),
                       movie.begin() + static_cast<long>(pos + len));
        }
        pos += len;
        if (code == 0) break;
    }
    if (pos < movie.size()) out.insert(out.end(), movie.begin() + static_cast<long>(pos), movie.end());
    if (!patched) return fail("no DoABC tag");
    const uint32_t total = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + 4, &total, 4);
    movie.swap(out);
    return true;
}

}  // namespace mc::game::swf
