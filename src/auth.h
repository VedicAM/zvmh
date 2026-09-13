#ifndef AUTH_H
#define AUTH_H

#include <string>
#include <vector>

namespace auth {

// A provider the client can authenticate against. `id` is the stable wire /
// auth.json identifier (lowercase, filesystem-friendly); `display_name` is what
// the TUI /connect dropdown shows the user.
struct ProviderInfo {
    std::string id;
    std::string display_name;
};

// The registry of providers the client can connect to. To add a provider,
// implement its Provider subclass in src/provider/ and add one entry here; the
// TUI dropdown, the auth.json round-trip and the wire handshake all follow
// from this list automatically.
const std::vector<ProviderInfo>& supported_providers();

// ~/.zvmh/auth.json
std::string auth_file_path();

// Reads the saved provider+api key. Returns false (with both outputs cleared)
// when nothing usable is stored.
bool load_credentials(std::string& provider_id, std::string& api_key);

// Persists provider+api key to ~/.zvmh/auth.json (0600, written atomically).
// Returns false on failure.
bool save_credentials(const std::string& provider_id, const std::string& api_key);

// Effective credentials: the OPENROUTER_API_KEY env var wins when set, falling
// back to ~/.zvmh/auth.json. active_provider_id() reflects only auth.json.
std::string active_api_key();
std::string active_provider_id();

}  // namespace auth

#endif