// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The app of a drop-in chain (thunder-app, bitnames-app...: doc/app.md of the drop-in repositories). Its window and its
// trusted dialog are the sidechain template's own (src/webui/window_*.cpp, dialog.cpp, bridge.cpp... compiled from the
// template at the pinned commit, libs/desktop/compat), its pages the template's interface kit with the drop-in's pages
// (app/chain of the chain's repository, core/app/ui). Its node is the drop-in node, a child process: the app is a client
// of the node's app RPC (bt/appui), as the unified wallet is of every sidechain's daemon:
//   - the pages' requests go to the node's webuihost; what needs the user's word comes back as a question and a one-time
//     token, asked here in the trusted dialog, sent again with the token on a yes;
//   - what the window does (the look, the clipboard, files, links, the title bar) is answered here;
//   - the node's events (webuievents) come to the page.
// The app is a user of the node of its own (an -rpcauth of this run, its password in memory only), limited by a
// whitelist to what the pages use (the chain's manifest).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bt::desktop {

struct ChainSpec {
    /** Thunder; thunder (the stem of the programs: <stem>, <stem>-app). */
    std::string name, stem;
    std::string version;
    /** The node's networks (LayerTwo's names), the default first. */
    std::vector<std::string> networks;
    /** The node's default LayerTwo RPC and P2P ports. */
    uint16_t rpc_port{0}, p2p_port{0};
    std::string default_mainchain_url{"http://localhost:50051"};
    /** The node's default data directory (LayerTwo's: <data_dir>/<stem>). */
    std::string default_datadir;
    /** What the app's user may call at the node: the chain's manifest (reads, confirmed and host methods, protocol). */
    std::vector<std::string> whitelist;
    /** The host methods of the node the pages call (forwarded; any other host method is the app's or refused). */
    std::vector<std::string> node_host_methods;
    std::string bugreport;
};

/** Run the app: returns the process's exit status. */
int Main(int argc, char** argv, const ChainSpec& spec);

} // namespace bt::desktop
