#ifndef CODEX_AUTH_H
#define CODEX_AUTH_H

#include <string>

struct CodexCredentials {
    std::string access_token;
    std::string refresh_token;
    std::string id_token;
};

CodexCredentials load_codex_credentials();

#endif
