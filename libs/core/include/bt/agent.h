// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Who we are: the drop-in's product name and version, set by the drop-in's CMake
// (BT_AGENT_NAME, BT_AGENT_VERSION before add_subdirectory(core)). The LayerTwo P2P
// protocol has no user-agent field, so the only place a peer can read it is the subject
// CN of our QUIC server certificate (bt/quic.h): "<Name> <version> (Backbone drop-in)".
#pragma once

#include <optional>
#include <string>

namespace bt {

const char* agent_name();    // e.g. "Thunder"
const char* agent_version(); // e.g. "1.0.0"
/** "<Name> <version>", e.g. "Thunder 1.0.0". */
std::string agent();

/** Suffix that marks our certificates' CN. */
inline constexpr const char* AGENT_CN_SUFFIX = " (Backbone drop-in)";
/** rcgen's default CN, what the upstream nodes present (and we, with an empty agent). */
inline constexpr const char* RCGEN_CN = "rcgen self signed cert";
/** Shown for an outbound peer whose certificate is not ours (upstream nodes). */
inline constexpr const char* PEER_NAME_UPSTREAM = "unknown (upstream)";
/** Shown for an inbound peer (clients present no certificate). */
inline constexpr const char* PEER_NAME_INBOUND = "unknown";

/** Our certificate CN for an agent string: agent + AGENT_CN_SUFFIX; RCGEN_CN if agent is empty. */
std::string agent_cert_cn(const std::string& agent);
/** A peer's name from its certificate CN: the agent part of one of our CNs (printable
 *  ASCII, at most 64 characters), else PEER_NAME_UPSTREAM (rcgen's CN, no certificate, anything else). */
std::string peer_name_from_cn(const std::optional<std::string>& cn);

} // namespace bt
