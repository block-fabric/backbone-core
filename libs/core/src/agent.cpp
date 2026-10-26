// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/agent.h>

#ifndef BT_AGENT_NAME
#define BT_AGENT_NAME "Backbone"
#endif
#ifndef BT_AGENT_VERSION
#define BT_AGENT_VERSION "0.0.0"
#endif

namespace bt {

const char* agent_name() { return BT_AGENT_NAME; }
const char* agent_version() { return BT_AGENT_VERSION; }
std::string agent() { return std::string(BT_AGENT_NAME) + " " + BT_AGENT_VERSION; }

std::string agent_cert_cn(const std::string& agent) { return agent.empty() ? std::string(RCGEN_CN) : agent + AGENT_CN_SUFFIX; }

std::string peer_name_from_cn(const std::optional<std::string>& cn)
{
    const std::string suffix = AGENT_CN_SUFFIX;
    if (!cn || cn->size() <= suffix.size() || cn->compare(cn->size() - suffix.size(), suffix.size(), suffix) != 0) return PEER_NAME_UPSTREAM;
    std::string name = cn->substr(0, cn->size() - suffix.size());
    if (name.size() > 64 || name.front() == ' ' || name.back() == ' ') return PEER_NAME_UPSTREAM;
    for (unsigned char ch : name)
        if (ch < 0x20 || ch > 0x7e) return PEER_NAME_UPSTREAM;
    return name;
}

} // namespace bt
