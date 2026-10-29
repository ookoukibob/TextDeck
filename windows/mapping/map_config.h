// ---------------------------------------------------------------------------
// map_config.h -- versioned JSON configuration: load, validate with readable
// field-level errors, migrate between versions, and build an Engine from it.
// ---------------------------------------------------------------------------
#pragma once

#include "map_core.h"
#include "map_engine.h"
#include "map_json.h"

#include <string>
#include <vector>

namespace map {

struct ConfigError {
    std::string field;    // JSON pointer-ish path, e.g. "profiles.default[0].script[2]"
    std::string message;  // what is wrong
    std::string toText() const { return field + ": " + message; }
};

// Current schema version accepted by this build.
constexpr int kConfigVersion = 1;

// Detect the declared version (defaults to 1 when absent).
int DetectVersion(const JsonPtr& root);

// Validate structure + field types + trigger/action semantics. Returns false
// and fills `errs` with every problem found (does not stop at the first).
bool ValidateConfig(const JsonPtr& root, std::vector<ConfigError>& errs);

// Migrate an older document forward to kConfigVersion in place. Returns false
// with an error if the version is unknown/newer than supported.
bool MigrateConfig(JsonPtr& root, std::string* err);

// Parse a JSON document, migrate if needed, validate, and load into `engine`.
// On validation failure returns false and reports every error (never loads a
// partially-broken config silently).
bool LoadConfigText(const std::string& json, Engine& engine,
                    std::vector<ConfigError>& errs);
bool LoadConfigFile(const std::string& path, Engine& engine,
                    std::vector<ConfigError>& errs);

// A ready-to-run example configuration that exercises several trigger kinds.
std::string ExampleConfigJson();

} // namespace map
