// map_config.cpp -- config validation / migration / loading. See map_config.h.
#include "map_config.h"

#include <cstdio>

namespace map {

namespace {

bool ValidTrigger(const std::string& t) {
    return t=="press"||t=="release"||t=="momentary"||t=="toggle"||t=="threshold" ||
           t=="long_press"||t=="hold_repeat"||t=="double_tap"||t=="multi_tap" ||
           t=="sequence"||t=="combo"||t=="cooldown"||t.empty();
}
bool ValidEventType(const std::string& t) {
    return t=="pressed"||t=="released"||t=="value"||t=="message"||t=="timer"||t.empty();
}
bool ValidAction(const std::string& a) {
    return a=="key_down"||a=="key_up"||a=="key_tap"||a=="key_combo"||a=="type_text" ||
           a=="delay"||a=="repeat"||a=="conditional"||a=="profile_switch"||a=="cancel_sequence";
}

void ValidateAction(const JsonPtr& a, const std::string& path, std::vector<ConfigError>& errs) {
    if (!a || !a->isObject()) { errs.push_back({path, "action is not an object"}); return; }
    std::string kind = a->get("action")->s();
    if (!ValidAction(kind)) { errs.push_back({path + ".action", "unknown action '" + kind + "'"}); return; }
    if (kind=="key_down"||kind=="key_up"||kind=="key_tap") {
        std::string key = a->get("key")->s();
        if (key.empty()) errs.push_back({path + ".key", "required for " + kind});
        else if (!FindKey(key)) errs.push_back({path + ".key", "unknown key '" + key + "'"});
    }
    if (kind=="key_combo") {
        JsonPtr keys = a->get("keys");
        if (!keys || !keys->isArray() || keys->arr.empty()) {
            errs.push_back({path + ".keys", "required non-empty array for key_combo"});
        } else {
            for (size_t i = 0; i < keys->arr.size(); ++i) {
                std::string k = keys->arr[i]->s();
                if (k.empty() || !FindKey(k))
                    errs.push_back({path + ".keys[" + std::to_string(i) + "]",
                                    "unknown key '" + k + "'"});
            }
        }
    }
    if (kind=="type_text" && a->get("text")->s().empty())
        errs.push_back({path + ".text", "required for type_text"});
    if (kind=="delay" && !a->get("ms")->isNumber())
        errs.push_back({path + ".ms", "required number for delay"});
    if (kind=="repeat" && !a->get("count")->isNumber())
        errs.push_back({path + ".count", "required number for repeat"});
    if (kind=="conditional" && a->get("if")->s().empty())
        errs.push_back({path + ".if", "required for conditional"});
    if (kind=="profile_switch" && a->get("profile")->s().empty())
        errs.push_back({path + ".profile", "required for profile_switch"});
    JsonPtr steps = a->get("steps");
    if (steps && steps->isArray())
        for (size_t i = 0; i < steps->arr.size(); ++i)
            ValidateAction(steps->arr[i], path + ".steps[" + std::to_string(i) + "]", errs);
}

void ValidateBinding(const JsonPtr& b, const std::string& path, std::vector<ConfigError>& errs) {
    if (!b || !b->isObject()) { errs.push_back({path, "binding is not an object"}); return; }
    if (b->get("id")->s().empty()) errs.push_back({path + ".id", "required non-empty string"});
    std::string trig = b->get("trigger")->s();
    if (!ValidTrigger(trig)) errs.push_back({path + ".trigger", "unknown trigger '" + trig + "'"});
    std::string type = b->get("type")->s();
    if (!ValidEventType(type)) errs.push_back({path + ".type", "unknown event type '" + type + "'"});

    if (trig == "sequence") {
        JsonPtr seq = b->get("sequence");
        if (!seq || !seq->isArray() || seq->arr.empty())
            errs.push_back({path + ".sequence", "required non-empty array for sequence trigger"});
    }
    if (trig == "combo") {
        JsonPtr c = b->get("combo");
        if (!c || !c->isArray() || c->arr.empty())
            errs.push_back({path + ".combo", "required non-empty array for combo trigger"});
    }
    if (trig == "multi_tap" && (!b->get("tapCount")->isNumber() || b->get("tapCount")->i() < 2))
        errs.push_back({path + ".tapCount", "must be >= 2 for multi_tap"});

    // Held-key names must resolve; an unknown name would silently no-op.
    std::string holdKey = b->get("holdKey")->s();
    if (!holdKey.empty() && !FindKey(holdKey))
        errs.push_back({path + ".holdKey", "unknown key '" + holdKey + "'"});
    std::string aboveKey = b->get("aboveKey")->s();
    if (!aboveKey.empty() && !FindKey(aboveKey))
        errs.push_back({path + ".aboveKey", "unknown key '" + aboveKey + "'"});

    JsonPtr script = b->get("script");
    if (script && script->isArray())
        for (size_t i = 0; i < script->arr.size(); ++i)
            ValidateAction(script->arr[i], path + ".script[" + std::to_string(i) + "]", errs);
}

} // namespace

int DetectVersion(const JsonPtr& root) {
    if (!root || !root->isObject()) return kConfigVersion;
    JsonPtr v = root->get("version");
    return v->isNumber() ? v->i() : 1;
}

bool ValidateConfig(const JsonPtr& root, std::vector<ConfigError>& errs) {
    if (!root || !root->isObject()) { errs.push_back({"", "root is not a JSON object"}); return false; }
    int ver = DetectVersion(root);
    if (ver > kConfigVersion) {
        errs.push_back({"version", "config version " + std::to_string(ver) +
                        " is newer than supported (" + std::to_string(kConfigVersion) + ")"});
        return false;
    }
    JsonPtr out = root->get("output");
    if (!out->isNull() && out->isString()) {
        std::string o = out->s();
        if (o != "hid" && o != "sendinput" && o != "record" && o != "default")
            errs.push_back({"output", "unknown output backend '" + o + "'"});
    }
    JsonPtr profs = root->get("profiles");
    if (!profs || !profs->isObject()) { errs.push_back({"profiles", "required object"}); return false; }
    for (const auto& p : profs->obj) {
        std::string pp = "profiles." + p.first;
        if (!p.second || !p.second->isArray()) {
            // profile may be { "bindings": [...] }
            JsonPtr bs = p.second ? p.second->get("bindings") : nullptr;
            if (bs && bs->isArray()) {
                for (size_t i = 0; i < bs->arr.size(); ++i)
                    ValidateBinding(bs->arr[i], pp + ".bindings[" + std::to_string(i) + "]", errs);
            } else {
                errs.push_back({pp, "profile must be an array of bindings or {bindings:[...]}"});
            }
            continue;
        }
        for (size_t i = 0; i < p.second->arr.size(); ++i)
            ValidateBinding(p.second->arr[i], pp + "[" + std::to_string(i) + "]", errs);
    }
    return errs.empty();
}

bool MigrateConfig(JsonPtr& root, std::string* err) {
    int ver = DetectVersion(root);
    if (ver > kConfigVersion) { if (err) *err = "config version newer than supported"; return false; }
    if (ver < kConfigVersion) {
        // v0/absent -> v1: stamp the version. No field renames yet.
        if (root && root->isObject()) root->obj["version"] = JsonValue::Number_(kConfigVersion);
    }
    return true;
}

bool LoadConfigText(const std::string& json, Engine& engine, std::vector<ConfigError>& errs) {
    std::string perr;
    JsonPtr root = ParseJson(json, &perr);
    if (!root) { errs.push_back({"", "JSON parse error: " + perr}); return false; }
    if (!MigrateConfig(root, &perr)) { errs.push_back({"version", perr}); return false; }
    if (!ValidateConfig(root, errs)) return false;
    std::string lerr;
    if (!engine.loadProfile(root, &lerr)) { errs.push_back({"profiles", lerr}); return false; }
    return true;
}

bool LoadConfigFile(const std::string& path, Engine& engine, std::vector<ConfigError>& errs) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { errs.push_back({"", "cannot open config file: " + path}); return false; }
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return LoadConfigText(s, engine, errs);
}

std::string ExampleConfigJson() {
    return R"({
  "version": 1,
  "output": "record",
  "profiles": {
    "default": [
      {
        "id": "space-on-button",
        "trigger": "momentary",
        "source": "xinput:0",
        "control": "a",
        "type": "pressed",
        "holdKey": "space"
      },
      {
        "id": "w-on-axis-threshold",
        "trigger": "threshold",
        "source": "xinput:0",
        "control": "ly",
        "type": "value",
        "threshold": 0.5,
        "hysteresis": 0.1,
        "aboveKey": "w"
      },
      {
        "id": "combo-on-message",
        "trigger": "press",
        "source": "udp",
        "control": "msg",
        "type": "message",
        "msgText": "launch",
        "script": [
          { "action": "key_combo", "keys": ["ctrl", "shift", "p"] }
        ]
      },
      {
        "id": "macro-on-double-tap",
        "trigger": "double_tap",
        "source": "xinput:0",
        "control": "b",
        "type": "pressed",
        "tapWindowMs": 300,
        "script": [
          { "action": "key_tap", "key": "a" },
          { "action": "delay", "ms": 50 },
          { "action": "key_tap", "key": "b" },
          { "action": "type_text", "text": "hi" }
        ]
      },
      {
        "id": "long-press-open",
        "trigger": "long_press",
        "source": "xinput:0",
        "control": "x",
        "type": "pressed",
        "longPressMs": 600,
        "script": [ { "action": "key_tap", "key": "f13" } ]
      },
      {
        "id": "sequence-secret",
        "trigger": "sequence",
        "source": "xinput:0",
        "seqTimeoutMs": 1500,
        "sequence": [
          { "control": "up", "type": "pressed" },
          { "control": "up", "type": "pressed" },
          { "control": "down", "type": "pressed" }
        ],
        "script": [ { "action": "key_tap", "key": "enter" } ]
      },
      {
        "id": "cross-device-combo",
        "trigger": "combo",
        "combo": ["xinput:0|y", "xinput:1|a"],
        "type": "pressed",
        "script": [ { "action": "key_combo", "keys": ["ctrl", "c"] } ]
      },
      {
        "id": "rate-limited-fire",
        "trigger": "cooldown",
        "source": "xinput:0",
        "control": "start",
        "type": "pressed",
        "cooldownMs": 500,
        "script": [ { "action": "key_tap", "key": "escape" } ]
      }
    ]
  }
})";
}

} // namespace map
