// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The host of a drop-in chain's pages, over the app RPC (rpcserver.h): the protocol of the sidechain template's
// remote host (src/webui/remotehost.h, HOST_API 1), so that our app of the chain and the unified wallet show the
// pages and ask for every spend exactly as they do for our template sidechains (doc/app.md of the drop-in
// repositories; doc/unified-app.md §13 of Chains/Backbone):
//
//   webuihost {kind, method, params, wallet?} ( "token" "passphrase" ["file tokens"] )
//       One request of the pages: kind "rpc" a read of the pages' policy (the chain's page reads), kind "host" a host
//       method (the chain's own, rpc.confirmed, app.hello/app.info/node.state). What needs the user's word is never
//       asked here: the answer is {confirm: <question>} with a one-time token; the app shows it in its own trusted
//       dialog and, on a yes, sends the very same request again with the token. The question is made again and the
//       call runs only if the token is that of exactly this request, this question and this wallet, unused, in time.
//   describeconfirmation "method" [params]
//       The question of a confirmed method (rpc.confirmed), described, never run, with the request to send.
//   webuievents ( since timeout )
//       The events for the pages (tip, peers, balance, transactions), in order, waited for at most `timeout` ms.
//   webuichosenfile "path" ( write overwrite )
//       A one-time token for a file the user chose in the app's own chooser (only the -chooser user grants one).
//
// Every method run by name inside a request (a read of kind "rpc", the inner method of rpc.confirmed) must be one the
// calling RPC user may call itself (its whitelist); a host method runs for a whitelisted user only when the whitelist
// names it, or every method of its privileges entry (Chain::privileges, as the template's HostPrivileges).
#pragma once

#include <bt/appui/rpcserver.h>

#include <univalue.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bt::appui {

/** The version of what this host answers (the template's remotehost.h HOST_API, app/scripts/pack.mjs HOST_API). */
inline constexpr int HOST_API{1};
inline constexpr std::chrono::seconds TOKEN_EXPIRY{300};
inline constexpr size_t MAX_TOKENS{64};
/** A question has at most so many rows (the template's dialog::MAX_ROWS). */
inline constexpr size_t MAX_ROWS{5000};

/** The bridge's error codes (the template's webui/bridge.h), what the pages and the apps read. */
enum HostCode : int {
    HOST_ERROR = -1,
    HOST_STOPPED = -9,
    HOST_NOT_READY = -28,
    HOST_INVALID_REQUEST = -32600,
    HOST_METHOD_NOT_FOUND = -32601,
    HOST_CANCELLED = -32800,
    HOST_AUTH_NEEDED = -32801,
    CONFIRMATION_REFUSED = -32802,
    HOST_NOT_ALLOWED = -32803,
};

/** A fact of a question: kind text, address, amount, total, heading, note (image: never from a drop-in). */
struct Row {
    std::string label, value, kind{"text"};
};

/** A question of the host (the template's Window::ConfirmDialog, as its DialogJson writes it). */
struct Question {
    std::string title, message, accept, cancel, details;
    std::string level{"warning"}; // info, warning, danger
    std::string kind;             // what kind of question (the app's limits of questions): the title when empty
    std::vector<Row> rows;
    bool passphrase{false};
    std::string passphrase_label, error;
};

UniValue question_json(const Question& q);
/** SHA-256 hex of "len:request" "len:question JSON": what a token binds (the template's Digest). */
std::string digest(const std::string& request, const Question& q);
/** Whether an object or array, at any depth, names a key twice. */
bool has_duplicate_keys(const UniValue& value);

/** What a host method throws to answer with an error of its own (the template's HostError). */
struct HostError : std::runtime_error {
    int code;
    HostError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
    explicit HostError(const std::string& m) : HostError(HOST_ERROR, m) {}
};

struct Answer {
    bool accepted{false};
    std::string passphrase;
};

/** One-time tokens of the questions described and not answered (the template's Bindings). */
class Bindings {
public:
    enum class Check { OK, UNKNOWN, EXPIRED, REQUEST_CHANGED, QUESTION_CHANGED, WALLET_CHANGED };
    static std::string text(Check c);
    std::string issue(const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now);
    Check take(const std::string& token, const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now);
    size_t size() const;

private:
    struct Entry {
        std::string request, digest, wallet_id;
        std::chrono::steady_clock::time_point expires;
        uint64_t order;
    };
    mutable std::mutex mu_;
    std::map<std::string, Entry> entries_;
    uint64_t next_{0};
};

/** The events of the pages, kept in order (the template's EventLog). */
class EventLog {
public:
    static constexpr size_t MAX_BATCHES{512};
    void add(std::string batch);
    /** {seq, batches: [json text], reset} after `since`, waited for at most `timeout` when there are none. */
    UniValue after(uint64_t since, std::chrono::milliseconds timeout);
    void stop();

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<uint64_t, std::string>> batches_;
    uint64_t seq_{0};
    bool stopped_{false};
};

/**
 * Events of the node for the pages, as the template's NodeEvents names them (tip, connections, alert,
 * wallet.loaded, wallet.balance, wallet.tx, ...): gathered for a moment and written to the log as one batch; an event
 * with a key replaces the one of the same key not sent yet (the tip, a balance).
 */
class EventHub {
public:
    explicit EventHub(EventLog& log);
    ~EventHub();
    void emit(const std::string& type, UniValue data, const std::string& key = {});
    void flush();
    void stop();

private:
    EventLog& log_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::pair<std::string, UniValue>> pending_;
    std::map<std::string, size_t> keyed_;
    bool stopped_{false};
    std::thread thread_;
};

/**
 * The files the user chose in the app's own chooser (the template's ChosenFiles): a path is taken only absolute and
 * canonical (no link, no ".."); the file (or, for one to be made, its folder) is opened when granted and kept open, and
 * used through what was opened: read from it, or written into it (a file to make: made in that very folder, never by
 * opening the path again). A file there already is written over only with `overwrite` (the chooser asked the user).
 */
class ChosenFiles {
public:
    static constexpr std::chrono::seconds EXPIRY{120};
    static constexpr size_t MAX{32};
    struct Held;
    struct File {
        std::string path;
        bool write{false}, overwrite{false};
        std::string user;
        std::shared_ptr<Held> held;
    };
    /** Why a path cannot be taken ("" when it can); with a folder, only a file directly in it. */
    static std::string problem(const std::string& path, const std::string& folder = {});
    /** A token for the file, the user's. Throws std::runtime_error with what is wrong. */
    std::string grant(const std::string& path, bool write, bool overwrite, const std::string& user, std::chrono::steady_clock::time_point now,
                      const std::string& folder = {});
    /** The file of a token for `user`, once. */
    std::optional<File> take(const std::string& token, const std::string& user, std::chrono::steady_clock::time_point now);
    /** Read a chosen file (at most `max` bytes) through what was opened. Throws std::runtime_error. */
    static std::string read(const File& f, size_t max);
    /** Write a chosen file through what was opened (0600 when made). Throws std::runtime_error. */
    static void write(const File& f, const std::string& data);

private:
    struct Entry {
        File file;
        std::chrono::steady_clock::time_point expires;
    };
    std::mutex mu_;
    std::map<std::string, Entry> entries_;
};

/** A request of the pages, read (the template's ParseRequest). Throws RpcError. */
struct Request {
    std::string kind, method;
    UniValue params;
    std::string wallet;
};
Request parse_request(const UniValue& request);

/** What a host method gets besides its parameters. */
struct HostEnv {
    /** The RPC user the request runs for. */
    std::string user;
    /**
     * Ask the user (a spend, a signature, a backup shown). Over RPC it does not ask: the first time it ends the request
     * with {confirm} (it throws, past the method's own handlers); sent again with the right token it answers yes.
     * Ask before anything is done that cannot be undone, and once per request.
     */
    std::function<Answer(const Question&)> ask;
    /** The file the user chose for this request at `path` (read or write): the chosen file, else throws. */
    std::function<ChosenFiles::File(const std::string& path, bool write)> file;
    /** Whether the caller may call a method (its whitelist): for host methods that run others by name. */
    std::function<bool(const std::string&)> may_call;
};

using HostFn = std::function<UniValue(const UniValue& params, HostEnv& env)>;

/** A method that pays, signs or changes what the wallet holds: asked (rpc.confirmed, describeconfirmation). */
struct Confirmed {
    /** Its parameters in their one form (a number as a number...): what the request carries. Throws HostError. */
    std::function<UniValue(const UniValue& params)> canonical;
    /** The question of these parameters. Throws HostError when they are not right. */
    std::function<Question(const UniValue& params)> describe;
    /** Run it, after a yes. */
    std::function<UniValue(const UniValue& params)> run;
};

/** What a drop-in chain plugs into its host. */
struct ChainHost {
    /** The chain's name (Thunder) and its stem (thunder), its network (betanet, regtest...). */
    std::string name, stem, network;
    /** app.info of the daemon (name, version, network, ...): the host adds hostApi, embedded, remote. */
    std::function<UniValue()> info;
    /** node.state: {connections, networkActive, warnings, tip: {...}, headerTip, wallets: [...]}. */
    std::function<UniValue()> snapshot;
    /** The identity of the wallet (a fingerprint of its seed; "" without one): bound into every token. */
    std::function<std::string()> wallet_id;
    /** The reads of the pages (kind "rpc"): names of app RPC methods. */
    std::set<std::string> page_reads;
    std::map<std::string, HostFn> host;
    std::map<std::string, Confirmed> confirmed;
    /** host method -> the methods it exercises (HostPrivileges). Missing: only when named in the whitelist. */
    std::map<std::string, std::vector<std::string>> privileges;
    /**
     * The read that stands for the wallet's amounts (getbalances): a caller that may not call it gets no wallet events
     * (wallet.balance, wallet.tx...) from webuievents and no "balances" in node.state.
     */
    std::string wallet_read{"getbalances"};
    /** The read that stands for the chain's state (node.state: the tip, the peers): its privilege. */
    std::string chain_read{"getblockchaininfo"};
    /**
     * The privilege each event type needs (webuievents): an event of a type not listed reaches nobody (but a caller that
     * may call everything). Empty: the template's events, the chain's to chain_read, the wallet's to wallet_read.
     */
    std::map<std::string, std::string> event_privileges;
};

struct HostOptions {
    /** The RPC user of the app that shows the pages (its -rpcauth user): the one that grants chosen files. */
    std::string chooser_user;
    /** Only files in this folder are granted (empty: any). */
    std::string file_dir;
};

/** Thrown out of a host method by HostEnv::ask over RPC (not a std::exception: no handler of the method takes it). */
struct NeedConfirmation {
    UniValue description;
};

class RemoteHost {
public:
    RemoteHost(ChainHost chain, HostOptions options, RpcServer& server);
    ~RemoteHost();
    RemoteHost(const RemoteHost&) = delete;
    RemoteHost& operator=(const RemoteHost&) = delete;

    EventHub& events() { return hub_; }
    /** The node stops: waits return, no request runs any more. */
    void stop();

    /** webuihost, for tests and for an app that runs the host in its own process. */
    UniValue run(const UniValue& request, const std::string& token, const std::string& passphrase, const UniValue& files, const std::string& user);
    UniValue describe(const std::string& method, const UniValue& params, const std::string& user);
    /** webuievents for a user: what the user may read of them (no wallet events without the wallet read). */
    UniValue events_for(uint64_t since, std::chrono::milliseconds timeout, const std::string& user);
    Bindings& bindings() { return bindings_; }
    /** HostPrivileges: whether `allowed` lets a user run a host method. */
    bool host_method_allowed(const std::string& method, const std::function<bool(const std::string&)>& allowed) const;

private:
    struct Context;
    UniValue call_rpc(const std::string& method, const UniValue& params, const std::string& wallet, Context& ctx);
    UniValue call_host(const std::string& method, const UniValue& params, Context& ctx);
    Answer ask(const Question& q, Context& ctx);
    UniValue info() const;
    static std::string canonical(const std::string& kind, const std::string& method, const UniValue& params, const std::string& wallet);

    ChainHost chain_;
    HostOptions options_;
    RpcServer& server_;
    Bindings bindings_;
    EventLog log_;
    EventHub hub_;
    ChosenFiles files_;
    std::atomic<bool> stopped_{false};
};

} // namespace bt::appui
