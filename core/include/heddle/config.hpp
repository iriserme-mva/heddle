#pragma once
#include <string>

#include "heddle/chain.hpp"

namespace heddle {
namespace config {

// Parse a Heddle chain config JSON document.
//
// Schema (see docs/CONFIG.md):
// {
//   "sample_rate": 48000,          // optional, default 44100
//   "block_size": 512,             // optional, default 512
//   "osc": {                       // optional; live app / bridge OSC server
//     "port": 9000,                // UDP port to bind (0 / absent = disabled)
//     "bind_host": "127.0.0.1"     // optional, default "127.0.0.1"
//   },
//   "chain": [
//     {
//       "id": "comp",              // required, unique
//       "plugin": "/path/Comp.vst3",// required
//       "plugin_id": "...",        // optional
//       "enabled": true,           // optional, default true
//       "params": { "Threshold": 0.5, "Ratio": 0.3 }   // name -> 0..1
//     }
//   ]
// }
//
// Returns false and fills *err on parse/validation failure.
bool parse(const std::string& jsonText, ChainConfig& out, std::string* err);

bool parseFile(const std::string& path, ChainConfig& out, std::string* err);

// Serialize back to JSON (stable key order, 2-space indent).
std::string dump(const ChainConfig& cfg);

// Merge: override wins for scalars and per-param values; slot lists are
// matched by id (slots only in base are kept, only in override are added).
ChainConfig merge(const ChainConfig& base, const ChainConfig& overrideCfg);

}  // namespace config
}  // namespace heddle
