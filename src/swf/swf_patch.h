#pragma once
// Patching Flash (Scaleform GFx, AS3) movies at load time, e.g. a game's menus.
//
// Scaleform menus are AS3 movies (*.gfx: an uncompressed SWF with a "GFX" signature). Each holds one DoABC tag with
// the compiled ActionScript. To add menu entries a game layer parses that ABC block, appends constants (strings,
// names) to its constant pool and prepends a few instructions to existing methods, then writes the movie back. All
// existing pool indices, method/class/trait records stay as they were, so the rest of the movie is untouched.
// Format: Adobe "ActionScript Virtual Machine 2 (AVM2) Overview" (public spec) and the SWF file format spec.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::swf {

// AVM2 constants we use.
namespace ns {
constexpr uint8_t kPackage = 0x16;  // PackageNamespace ("" = public)
constexpr uint8_t kPrivate = 0x05;
}  // namespace ns

class Abc {
public:
    bool Parse(const uint8_t* data, size_t size, std::string* err);
    std::vector<uint8_t> Write() const;

    // ---- constant pool (find or append) ----
    uint32_t String(std::string_view s);
    uint32_t Namespace(uint8_t kind, std::string_view name);
    uint32_t QName(uint8_t nsKind, std::string_view nsName, std::string_view name);
    uint32_t Public(std::string_view name) { return QName(ns::kPackage, "", name); }
    uint32_t RuntimeIndexName();  // MultinameL over the public namespace: obj[expr]
    // A name for property access on objects of any class: an existing Multiname (namespace set) with that local
    // name, else a public QName (appended if missing).
    uint32_t Name(std::string_view name);

    // ---- classes, traits, methods ----
    int FindClass(std::string_view nsName, std::string_view name) const;  // instance index or -1
    // Multiname index of the class's trait called `name` (instance traits, then static traits); 0 if none.
    uint32_t TraitName(int cls, std::string_view name) const;
    // Method index of an instance method trait; -1 if none.
    int TraitMethod(int cls, std::string_view name) const;
    uint32_t LocalCount(int method) const;  // of its body (0 if no body)

    // Prepends code to a method body. extraStack/extraLocals are added to max_stack/local_count; exception
    // ranges are shifted. False if the method has no body.
    bool Prepend(int method, const std::vector<uint8_t>& code, uint32_t extraStack, uint32_t extraLocals);

    std::string NameOf(uint32_t multiname) const;

private:
    struct Ns {
        uint8_t kind;
        uint32_t name;
    };
    struct Multiname {
        uint8_t kind;
        uint32_t a = 0, b = 0;           // QName: ns, name; RTQName: name; Multiname: name, nsset; MultinameL: nsset
        std::vector<uint32_t> params;    // TypeName
    };
    struct Trait {
        uint32_t name;
        uint8_t kind;    // low 4 bits
        uint32_t index;  // method index for method/getter/setter/function, class index for class traits
    };
    struct Body {
        uint32_t method, maxStack, localCount, initScope, maxScope;
        std::vector<uint8_t> code;
        struct Exc {
            uint32_t from, to, target, type, name;
        };
        std::vector<Exc> exceptions;
        std::vector<uint8_t> traits;  // raw trait records (count + entries)
    };

    std::vector<uint8_t> _ints, _uints, _doubles;  // raw pool sections incl. their counts
    std::vector<std::string> _strings;             // [0] unused
    std::vector<Ns> _namespaces;                   // [0] unused
    std::vector<std::vector<uint32_t>> _nsSets;    // [0] unused
    std::vector<Multiname> _multinames;            // [0] unused
    std::vector<uint8_t> _middle;                  // methods .. scripts, verbatim
    std::vector<std::vector<Trait>> _instTraits, _classTraits;
    std::vector<uint32_t> _instNames;
    std::vector<Body> _bodies;
    uint16_t _minor = 16, _major = 46;
};

// Builds AVM2 bytecode with labels. Branch offsets are resolved by Finish().
class Asm {
public:
    explicit Asm(Abc& abc) : _abc(abc) {}

    Asm& Op(uint8_t op);
    Asm& U30(uint32_t v);
    Asm& GetLocal(uint32_t r);
    Asm& SetLocal(uint32_t r);
    Asm& PushString(std::string_view s);
    Asm& PushInt(int v);  // pushbyte / pushshort
    Asm& GetProperty(uint32_t mn);
    Asm& SetProperty(uint32_t mn);
    Asm& GetLex(uint32_t mn);
    Asm& FindPropStrict(uint32_t mn);
    Asm& CallProp(uint32_t mn, uint32_t argc);
    Asm& CallPropVoid(uint32_t mn, uint32_t argc);
    Asm& ConstructProp(uint32_t mn, uint32_t argc);

    int Label();               // new unbound label
    Asm& Bind(int label);      // binds at the current position (emits no instruction)
    Asm& LoopHead(int label);  // binds and emits the AVM2 'label' instruction (target of a backward jump)
    Asm& Jump(uint8_t branchOp, int label);

    // Resolves branches; empty result (and a log line) on an unbound label.
    std::vector<uint8_t> Finish();

private:
    Abc& _abc;
    std::vector<uint8_t> _code;
    std::vector<long> _labels;  // position or -1
    struct Fix {
        size_t at;  // offset of the s24
        int label;
    };
    std::vector<Fix> _fixes;
};

// AVM2 opcodes used by our patches.
namespace op {
constexpr uint8_t kLabel = 0x09, kIfNlt = 0x0C, kJump = 0x10, kIfTrue = 0x11, kIfFalse = 0x12, kIfEq = 0x13,
                  kIfNe = 0x14, kIfLt = 0x15, kIfLe = 0x16, kIfGt = 0x17, kIfGe = 0x18, kIfStrictEq = 0x19,
                  kIfStrictNe = 0x1A, kPopScope = 0x1D, kPushNull = 0x20, kPushTrue = 0x26, kPushFalse = 0x27,
                  kPop = 0x29, kDup = 0x2A, kSwap = 0x2B, kPushScope = 0x30, kReturnVoid = 0x47, kNewArray = 0x56,
                  kNewObject = 0x55, kConvertS = 0x70, kConvertI = 0x73, kConvertU = 0x74, kCoerceS = 0x85,
                  kIncrementI = 0xC0, kAdd = 0xA0, kSubtract = 0xA1, kIncLocalI = 0xC2, kGetLocal0 = 0xD0,
                  kGetLocal1 = 0xD1;
}  // namespace op

// Rewrites a SWF/GFX movie: finds its DoABC tag(s), lets `patch` modify the ABC, writes the movie back (lengths
// fixed up). False (with *err) when the file is not an uncompressed SWF/GFX, has no DoABC tag, or patch fails.
bool PatchMovie(std::vector<uint8_t>& movie, const std::function<bool(Abc&, std::string*)>& patch, std::string* err);

}  // namespace mc::swf
