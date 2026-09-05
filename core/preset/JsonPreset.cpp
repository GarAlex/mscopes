//
// JsonPreset.cpp — see JsonPreset.h.
//
#include "JsonPreset.h"
#include "Presets.h"
#include "EffectList.h"
#include "Superscope.h"
#include "DynamicMovement.h"
#include "ScriptedTrans.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace viz {

static const char* kListKey = "effect_list";

// ---------------------------------------------------------------------------
//  Effect instance → registry key (via the display name, which is unique)
// ---------------------------------------------------------------------------
static const std::map<std::string, std::string>& nameToKey()
{
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> r;
        for (const auto& [key, factory] : effectRegistry())
            r[factory()->name()] = key;
        return r;
    }();
    return m;
}

// ---------------------------------------------------------------------------
//  Writer
// ---------------------------------------------------------------------------
static void jsonEscape(std::string& out, const std::string& s)
{
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    out += '"';
}

static void jsonNumber(std::string& out, double v)
{
    char buf[32];
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        snprintf(buf, sizeof buf, "%.0f", v);
    else
        snprintf(buf, sizeof buf, "%.9g", v);   // floats round-trip at 9 digits
    out += buf;
}

static void writeEffect(std::string& out, Effect* e, int indent)
{
    const std::string ind(indent, ' ');
    auto* list = dynamic_cast<EffectListEffect*>(e);

    auto it = nameToKey().find(e->name());
    std::string key = list ? kListKey
                    : (it != nameToKey().end() ? it->second : "");

    out += ind + "{\"key\": ";
    jsonEscape(out, key);
    out += ", \"enabled\": ";
    out += e->enabled ? "true" : "false";

    auto ps = e->params();
    if (!ps.empty()) {
        out += ",\n" + ind + " \"params\": {";
        bool first = true;
        for (const auto& p : ps) {
            if (!first) out += ", ";
            first = false;
            jsonEscape(out, p.key);
            out += ": ";
            jsonNumber(out, *p.value);
        }
        out += "}";
    }

    std::string si, sf, sb, sp;
    bool scripted = false;
    if (auto* s = dynamic_cast<SuperscopeEffect*>(e)) { s->getScripts(si, sf, sb, sp); scripted = true; }
    else if (auto* s = dynamic_cast<DynamicMovementEffect*>(e)) { s->getScripts(si, sf, sb, sp); scripted = true; }
    else if (auto* s = dynamic_cast<ScriptedEffectBase*>(e)) { s->getScripts(si, sf, sb, sp); scripted = true; }
    if (scripted) {
        out += ",\n" + ind + " \"scripts\": {\"init\": ";
        jsonEscape(out, si);
        out += ", \"frame\": ";
        jsonEscape(out, sf);
        out += ", \"beat\": ";
        jsonEscape(out, sb);
        out += ", \"point\": ";
        jsonEscape(out, sp);
        out += "}";
    }

    if (list) {
        out += ",\n" + ind + " \"children\": [";
        for (size_t i = 0; i < list->childCount(); ++i) {
            out += (i ? ",\n" : "\n");
            writeEffect(out, list->childAt(i), indent + 2);
        }
        out += "\n" + ind + " ]";
    }
    out += "}";
}

std::string saveJsonPreset(EffectHost& host, const std::string& name)
{
    std::string out = "{\"format\": \"mscopes-preset\", \"version\": 1,\n \"name\": ";
    jsonEscape(out, name);
    if (host.inheritCanvas) out += ",\n \"canvas\": \"inherit\"";
    out += ",\n \"effects\": [";
    for (size_t i = 0; i < host.count(); ++i) {
        out += (i ? ",\n" : "\n");
        writeEffect(out, host.at(i), 2);
    }
    out += "\n ]";
    if (!host.paramBindings().empty()) {
        out += ",\n \"bindings\": [";
        bool first = true;
        for (const auto& b : host.paramBindings()) {
            out += first ? "\n" : ",\n";
            first = false;
            char buf[64];
            snprintf(buf, sizeof buf, "  {\"effect\": %zu, \"param\": ", b.effectIndex);
            out += buf;
            jsonEscape(out, b.paramKey);
            snprintf(buf, sizeof buf, ", \"reg\": %d}", b.reg);
            out += buf;
        }
        out += "\n ]";
    }
    out += "}\n";
    return out;
}

// ---------------------------------------------------------------------------
//  Minimal JSON parser (objects/arrays/strings/numbers/bools/null)
// ---------------------------------------------------------------------------
namespace {
struct JVal {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const std::string& k) const {
        for (const auto& [key, v] : obj) if (key == k) return &v;
        return nullptr;
    }
};

struct JParser {
    const char* p;
    const char* end;
    std::string err;

    JParser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    void ws() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    bool parse(JVal& out) {
        ws();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') return object(out);
        if (c == '[') return array(out);
        if (c == '"') { out.kind = JVal::STR; return string(out.str); }
        if (c == 't' || c == 'f') {
            out.kind = JVal::BOOL;
            if (end - p >= 4 && !strncmp(p, "true", 4)) { out.b = true; p += 4; return true; }
            if (end - p >= 5 && !strncmp(p, "false", 5)) { out.b = false; p += 5; return true; }
            return fail("bad literal");
        }
        if (c == 'n') {
            if (end - p >= 4 && !strncmp(p, "null", 4)) { out.kind = JVal::NUL; p += 4; return true; }
            return fail("bad literal");
        }
        char* num_end = nullptr;
        out.num = std::strtod(p, &num_end);
        if (num_end == p) return fail("bad value");
        out.kind = JVal::NUM;
        p = num_end;
        return true;
    }

    bool string(std::string& s) {
        if (*p != '"') return fail("expected string");
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) return fail("bad escape");
                switch (*p) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        if (end - p < 5) return fail("bad \\u");
                        unsigned v = 0;
                        for (int i = 1; i <= 4; ++i) {
                            char h = p[i]; v <<= 4;
                            if (h >= '0' && h <= '9') v |= h - '0';
                            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                            else return fail("bad \\u");
                        }
                        // basic-plane only; encode as UTF-8
                        if (v < 0x80) s += (char)v;
                        else if (v < 0x800) {
                            s += (char)(0xC0 | (v >> 6));
                            s += (char)(0x80 | (v & 0x3F));
                        } else {
                            s += (char)(0xE0 | (v >> 12));
                            s += (char)(0x80 | ((v >> 6) & 0x3F));
                            s += (char)(0x80 | (v & 0x3F));
                        }
                        p += 4;
                        break;
                    }
                    default: return fail("bad escape");
                }
                ++p;
            } else s += *p++;
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }

    bool object(JVal& out) {
        out.kind = JVal::OBJ;
        ++p; ws();
        if (p < end && *p == '}') { ++p; return true; }
        for (;;) {
            ws();
            std::string key;
            if (!string(key)) return false;
            ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            JVal v;
            if (!parse(v)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool array(JVal& out) {
        out.kind = JVal::ARR;
        ++p; ws();
        if (p < end && *p == ']') { ++p; return true; }
        for (;;) {
            JVal v;
            if (!parse(v)) return false;
            out.arr.push_back(std::move(v));
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return true; }
            return fail("expected ',' or ']'");
        }
    }
};
} // anonymous namespace

// ---------------------------------------------------------------------------
//  Loader
// ---------------------------------------------------------------------------
static std::unique_ptr<Effect> buildEffect(const JVal& j, std::string* err)
{
    const JVal* keyV = j.get("key");
    if (!keyV || keyV->kind != JVal::STR) {
        if (err) *err = "effect entry missing \"key\"";
        return nullptr;
    }
    const std::string& key = keyV->str;

    std::unique_ptr<Effect> e;
    if (key == kListKey) {
        auto list = std::make_unique<EffectListEffect>();
        if (const JVal* ch = j.get("children"); ch && ch->kind == JVal::ARR) {
            for (const auto& c : ch->arr) {
                auto sub = buildEffect(c, err);
                if (!sub) return nullptr;
                list->addChild(std::move(sub));
            }
        }
        e = std::move(list);
    } else {
        auto it = effectRegistry().find(key);
        if (it == effectRegistry().end()) {
            if (err) *err = "unknown effect key \"" + key + "\"";
            return nullptr;
        }
        e = it->second();
    }

    if (const JVal* en = j.get("enabled"); en && en->kind == JVal::BOOL)
        e->enabled = en->b;

    if (const JVal* sc = j.get("scripts"); sc && sc->kind == JVal::OBJ) {
        auto s = [&](const char* k) {
            const JVal* v = sc->get(k);
            return (v && v->kind == JVal::STR) ? v->str : std::string();
        };
        std::string si = s("init"), sf = s("frame"), sb = s("beat"), sp = s("point");
        if (auto* fx = dynamic_cast<SuperscopeEffect*>(e.get())) fx->setScripts(si, sf, sb, sp);
        else if (auto* fx = dynamic_cast<DynamicMovementEffect*>(e.get())) fx->setScripts(si, sf, sb, sp);
        else if (auto* fx = dynamic_cast<ScriptedEffectBase*>(e.get())) fx->setScripts(si, sf, sb, sp);
    }

    // params AFTER scripts: setScripts may reset mode params (e.g. example=-1).
    // Assign verbatim (not via setParam) — .avs decoders legitimately set
    // values outside the UI slider ranges and those must survive.
    if (const JVal* ps = j.get("params"); ps && ps->kind == JVal::OBJ) {
        auto bound = e->params();
        for (const auto& [k, v] : ps->obj) {
            if (v.kind != JVal::NUM) continue;
            for (auto& p : bound)
                if (k == p.key) { *p.value = (float)v.num; break; }
        }
    }

    return e;
}

bool loadJsonPreset(EffectHost& host, const std::string& json, std::string* err)
{
    JParser parser(json);
    JVal root;
    if (!parser.parse(root) || root.kind != JVal::OBJ) {
        if (err) *err = parser.err.empty() ? "not a JSON object" : parser.err;
        return false;
    }
    const JVal* fmt = root.get("format");
    // "winamp-viz-preset" was the tag before the project was named.
    if (!fmt || fmt->kind != JVal::STR ||
        (fmt->str != "mscopes-preset" && fmt->str != "winamp-viz-preset")) {
        if (err) *err = "not an mscopes-preset file";
        return false;
    }
    const JVal* fx = root.get("effects");
    if (!fx || fx->kind != JVal::ARR) {
        if (err) *err = "missing \"effects\" array";
        return false;
    }

    const JVal* cv = root.get("canvas");
    bool inherit = cv && cv->kind == JVal::STR && cv->str == "inherit";

    resetSharedState();
    host.clearEffects(!inherit);
    host.inheritCanvas = inherit;
    for (const auto& j : fx->arr) {
        auto e = buildEffect(j, err);
        if (!e) { host.clearEffects(); return false; }
        host.add(std::move(e));
    }

    if (const JVal* bs = root.get("bindings"); bs && bs->kind == JVal::ARR)
        for (const auto& b : bs->arr) {
            const JVal* e = b.get("effect");
            const JVal* p = b.get("param");
            const JVal* r = b.get("reg");
            if (e && e->kind == JVal::NUM && p && p->kind == JVal::STR &&
                r && r->kind == JVal::NUM)
                host.setBinding((size_t)e->num, p->str,
                                std::clamp((int)r->num, 0, 99));
        }
    return true;
}

// ---------------------------------------------------------------------------
//  File wrappers
// ---------------------------------------------------------------------------
bool saveJsonPresetFile(EffectHost& host, const std::string& name,
                        const std::string& path)
{
    std::string text = saveJsonPreset(host, name);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return n == text.size();
}

bool loadJsonPresetFile(EffectHost& host, const std::string& path,
                        std::string* err)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return loadJsonPreset(host, text, err);
}

} // namespace viz
