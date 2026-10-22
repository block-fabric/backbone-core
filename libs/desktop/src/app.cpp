// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The app of a drop-in chain: the template's window, bridge and trusted dialog, the drop-in node as a child, the pages'
// requests to its app RPC (bt/desktop/app.h). What the window does is answered here as the template's app.cpp answers
// it (the same rules: the clipboard only after a gesture, files only those chosen, a link not the app's asked first,
// questions bounded); what the chain does is the node's, asked in the trusted dialog when it pays or shows a secret.
#include <bitcoin-build-config.h>

#include <bt/desktop/app.h>
#include <bt/desktop/child.h>

#include <bt/appui/auth.h>
#include <bt/appui/rpcserver.h>
#include <bt/log.h>

#include <logging.h>
#include <support/cleanse.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/threadnames.h>
#include <webui/appconfig.h>
#include <webui/assets.h>
#include <webui/bridge.h>
#include <webui/chainconfig.h>
#include <webui/dialog.h>
#include <webui/events.h>
#include <webui/guard.h>
#include <webui/i18n.h>
#include <webui/quitguard.h>
#include <webui/singleinstance.h>
#include <webui/window.h>
#include <webui/windowframe.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

using namespace std::chrono_literals;

namespace bt::desktop {

using webui::Bridge;
using webui::EventHub;
using webui::HostError;
using webui::Window;
namespace dialog = webui::dialog;

namespace {
constexpr int MAX_QUESTIONS_WAITING{4};
constexpr size_t MAX_DECLINED{3};
constexpr int64_t DECLINED_WINDOW_S{60};
constexpr int64_t CHOSEN_FILE_S{10 * 60};
constexpr int64_t CLIPBOARD_WRITE_MS{5000};
constexpr int64_t CLIPBOARD_READ_MS{2000};
constexpr size_t MAX_CLIPBOARD_READS{10};
constexpr size_t MAX_CLIPBOARD_BYTES{1'000'000};
constexpr int CONFIRMATION_REFUSED{-32802};
//! How long the app waits for the node's answer to a request of the pages (a deposit waits for the mainchain).
constexpr int REQUEST_TIMEOUT_MS{600'000};

int64_t SteadySeconds() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

const UniValue NULLV;
const UniValue& Param(const UniValue& params, const std::string& key) { return params.isObject() ? params.find_value(key) : NULLV; }

std::string ParamStr(const UniValue& params, const std::string& key)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr()) throw HostError(webui::BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return value.get_str();
}

std::string NetworkLabel(const std::string& network) { return network.empty() || network == "betanet" ? "" : network; }

dialog::Row::Kind RowKind(const std::string& name)
{
    using K = dialog::Row::Kind;
    for (const K k : {K::TEXT, K::ADDRESS, K::AMOUNT, K::TOTAL, K::HEADING, K::NOTE}) {
        if (dialog::RowKindName(k) == name) return k;
    }
    return K::TEXT;
}

dialog::Level LevelOf(const std::string& name)
{
    if (name == "info") return dialog::Level::INFO;
    if (name == "danger") return dialog::Level::DANGER;
    return dialog::Level::WARNING;
}

std::string Str(const UniValue& o, const std::string& key)
{
    const UniValue& v{Param(o, key)};
    return v.isStr() ? v.get_str() : std::string{};
}

/** A free port on 127.0.0.1, near `wanted` when it is free. */
uint16_t FreePort(uint16_t wanted)
{
#ifdef _WIN32
    // Winsock before the first socket of this process (the window's own code opens none).
    static const bool wsa{[] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }()};
    if (!wsa) return 0;
#endif
    for (int attempt{0}; attempt < 2; ++attempt) {
#ifdef _WIN32
        SOCKET s{socket(AF_INET, SOCK_STREAM, 0)};
#else
        int s{socket(AF_INET, SOCK_STREAM, 0)};
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(attempt == 0 ? wanted : 0);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        uint16_t got{0};
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            sockaddr_in b{};
            socklen_t len{sizeof(b)};
            getsockname(s, reinterpret_cast<sockaddr*>(&b), &len);
            got = ntohs(b.sin_port);
        }
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
        if (got) return got;
    }
    return 0;
}

std::string Tail(const std::string& path, size_t lines)
{
    std::ifstream f(fs::PathFromString(path));
    std::deque<std::string> last;
    std::string line;
    while (std::getline(f, line)) {
        last.push_back(line);
        if (last.size() > lines) last.pop_front();
    }
    std::string out;
    for (const auto& l : last) out += l + "\n";
    return out;
}

std::string DefaultDatadir(const std::string& stem)
{
#ifdef _WIN32
    const char* app{std::getenv("APPDATA")};
    return std::string(app ? app : ".") + "\\" + stem;
#elif defined(__APPLE__)
    const char* home{std::getenv("HOME")};
    return std::string(home ? home : ".") + "/Library/Application Support/" + stem;
#else
    if (const char* x{std::getenv("XDG_DATA_HOME")}; x && *x) return std::string(x) + "/" + stem;
    const char* home{std::getenv("HOME")};
    return std::string(home ? home : ".") + "/.local/share/" + stem;
#endif
}

struct Args {
    std::map<std::string, std::string> values;
    std::vector<std::string> node_args;
    bool has(const std::string& k) const { return values.contains(k); }
    std::string get(const std::string& k, const std::string& fallback = {}) const
    {
        auto it{values.find(k)};
        return it == values.end() ? fallback : it->second;
    }
};
} // namespace

class App
{
public:
    App(const ChainSpec& spec, Args args) : m_spec(spec), m_args(std::move(args))
    {
        m_devtools = m_args.has("webuidevtools");
#ifdef WEBUI_TEST_HOOKS
        m_test_script = m_args.get("webuitestscript");
        m_test_shots = m_args.get("webuitestshots");
#endif
    }

    int Run();

private:
    // ---- the node
    struct NodeSettings {
        std::string network, mainchain_url, datadir;
        uint16_t rpc_port{0}, p2p_port{0};
        std::vector<std::string> peers;
    };
    NodeSettings Settings();
    UniValue SettingsJson();
    /** LayerTwo's data directory of the node: the spec's, or <data_dir>/<datadir_name or stem>. */
    std::string DefaultNodeDatadir() const
    {
        if (!m_spec.default_datadir.empty()) return m_spec.default_datadir;
        return DefaultDatadir(m_spec.datadir_name.empty() ? m_spec.stem : m_spec.datadir_name);
    }
    void NodeThread();
    bool StartNode(std::string& err);
    void StopNode();
    void EventsThread(uint64_t generation);
    UniValue NodeCall(const std::string& method, const UniValue& params, int timeout_ms = 30'000);
    /** A request of the pages at the node (webuihost): its question asked here, sent again with the token on a yes. */
    UniValue NodeRequest(const std::string& kind, const std::string& method, const UniValue& params, const std::string& wallet);
    UniValue FileTokens(const UniValue& params, std::vector<std::string>& used);
    /** Whether the app RPC that answers is our node's: it proves it holds the cookie of the node's data folder. */
    bool ServerIsOurNode(const std::string& datadir, std::string& err);

    // ---- the window
    void RegisterHostMethods();
    UniValue Info();
    void SetPhase(const std::string& phase, UniValue data = UniValue{UniValue::VOBJ});
    void DeliverEvents(const std::string& batch_json);
    void UpdateTitle();
    UniValue NetworkPrefs();
    std::string Network();
    bool PrefFlag(const std::string& key, bool fallback);
    UniValue WindowState();
    void RequestShutdown();
    void QuitRequested();
    void CloseRequested();
    std::optional<std::string> Choose(const Window::FileDialog& dialog);
    bool TakeChosenFile(const std::string& path, bool write);
    dialog::Request DialogRequest(const Window::ConfirmDialog& confirm);
    dialog::Answer AskHost(const Window::ConfirmDialog& confirm);
    bool Confirm(const Window::ConfirmDialog& confirm) { return AskHost(confirm).accepted; }
    Window::ConfirmDialog Question(const UniValue& description);
    void Fatal(const std::string& title, const std::string& message, const std::vector<std::string>& details);

    const ChainSpec m_spec;
    const Args m_args;
    bool m_devtools{false};
    std::string m_test_script, m_test_shots;

    std::unique_ptr<webui::AppConfig> m_config;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<EventHub> m_hub;
    std::unique_ptr<Bridge> m_bridge;
    std::unique_ptr<webui::SingleInstance> m_instance;
    webui::QuitGuard m_quit_guard;
    std::thread m_node_thread;

    Mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_phase{"loading"};
    UniValue m_phase_data{UniValue::VOBJ};
    std::string m_language;
    std::string m_wallet_title;
    bool m_shutdown_requested{false};
    bool m_restart_node{false};
    bool m_page_ready{false};
    std::optional<int> m_exit_code;
    UniValue m_node_info;
    // The node's app RPC: where, as whom (an -rpcauth of this run; the password in memory only).
    uint16_t m_node_port{0};
    std::string m_user;
    std::string m_password;
    bool m_node_ready{false};
    uint64_t m_generation{0};
    std::thread m_events_thread;
    std::unique_ptr<Child> m_child;
    std::string m_node_log;
    // Files chosen in the chooser: when, and for writing.
    struct Chosen {
        int64_t when;
        bool write;
        bool overwrite;
    };
    std::map<std::string, Chosen> m_chosen;
    std::map<std::string, int64_t> m_readable, m_writable;
    std::map<int64_t, UniValue> m_dialogs;
    std::set<int64_t> m_informs;
    std::map<int64_t, UniValue> m_answers;
    int64_t m_next_dialog{1};
    struct QuestionKind {
        bool waiting{false};
        std::deque<int64_t> declined;
        bool refused{false};
    };
    std::map<std::string, QuestionKind> m_question_kinds;
    int m_questions_waiting{0};
    std::atomic<int> m_host_questions{0};
    uint64_t m_clipboard_read_gesture{0};
    uint64_t m_clipboard_write_gesture{0};
    std::deque<int64_t> m_clipboard_reads;
    std::vector<UniValue> m_notifications;
#ifdef WEBUI_TEST_HOOKS
    enum class TestConfirm { YES, NO, SHOW, FALLBACK };
    std::deque<TestConfirm> m_test_confirms;
    std::optional<TestConfirm> m_test_confirm_always;
    std::vector<Window::ConfirmDialog> m_test_confirmed;
    std::deque<UniValue> m_test_files;
#endif
};

// ------------------------------------------------------------------ the settings of the node
App::NodeSettings App::Settings()
{
    NodeSettings s;
    const UniValue saved{m_config->Value("node")};
    auto pick = [&](const std::string& arg, const std::string& key, const std::string& fallback) {
        if (m_args.has(arg)) return m_args.get(arg);
        const UniValue& v{Param(saved, key)};
        return v.isStr() && !v.get_str().empty() ? v.get_str() : fallback;
    };
    s.network = pick("network", "network", m_spec.networks.empty() ? std::string{"betanet"} : m_spec.networks.front());
    if (std::find(m_spec.networks.begin(), m_spec.networks.end(), s.network) == m_spec.networks.end()) s.network = m_spec.networks.front();
    s.mainchain_url = pick("mainchain-grpc-url", "mainchainUrl", m_spec.default_mainchain_url);
    s.datadir = pick("datadir", "datadir", DefaultNodeDatadir());
    auto port = [&](const std::string& arg, const std::string& key, uint16_t fallback) -> uint16_t {
        std::string v{m_args.get(arg)};
        if (v.empty() && Param(saved, key).isNum()) return static_cast<uint16_t>(Param(saved, key).getInt<int>());
        const auto n{ToIntegral<uint16_t>(v)};
        return n && *n ? *n : fallback;
    };
    s.rpc_port = port("rpc-port", "rpcPort", m_spec.rpc_port);
    s.p2p_port = port("p2p-port", "p2pPort", m_spec.p2p_port);
    if (const UniValue& peers{Param(saved, "peers")}; peers.isArray()) {
        for (const UniValue& p : peers.getValues()) {
            if (p.isStr() && s.peers.size() < 32) s.peers.push_back(p.get_str());
        }
    }
    return s;
}

UniValue App::SettingsJson()
{
    const NodeSettings s{Settings()};
    UniValue o{UniValue::VOBJ};
    o.pushKV("network", s.network);
    UniValue nets{UniValue::VARR};
    for (const auto& n : m_spec.networks) nets.push_back(n);
    o.pushKV("networks", nets);
    o.pushKV("mainchainUrl", s.mainchain_url);
    o.pushKV("defaultMainchainUrl", m_spec.default_mainchain_url);
    o.pushKV("datadir", s.datadir);
    o.pushKV("defaultDatadir", DefaultNodeDatadir());
    o.pushKV("rpcPort", s.rpc_port);
    o.pushKV("p2pPort", s.p2p_port);
    UniValue peers{UniValue::VARR};
    for (const auto& p : s.peers) peers.push_back(p);
    o.pushKV("peers", peers);
    UniValue fixed{UniValue::VARR};
    for (const char* k : {"network", "mainchain-grpc-url", "datadir", "rpc-port", "p2p-port"}) {
        if (m_args.has(k)) fixed.push_back(k);
    }
    o.pushKV("fixedByCommandLine", fixed);
    o.pushKV("log", m_node_log);
    return o;
}

// ------------------------------------------------------------------ the node
UniValue App::NodeCall(const std::string& method, const UniValue& params, int timeout_ms)
{
    uint16_t port;
    std::string user, password;
    {
        LOCK(m_mutex);
        if (!m_node_port) throw HostError(webui::BRIDGE_NOT_READY, "The node is not running");
        port = m_node_port;
        user = m_user;
        password = m_password;
    }
    try {
        UniValue r{bt::appui::rpc_call("127.0.0.1", port, user, password, method, params, "/", timeout_ms)};
        memory_cleanse(password.data(), password.size());
        return r;
    } catch (const bt::appui::RpcError& e) {
        memory_cleanse(password.data(), password.size());
        throw HostError(e.code, e.what());
    } catch (const std::exception& e) {
        memory_cleanse(password.data(), password.size());
        throw HostError(webui::BRIDGE_NOT_READY, strprintf("The node does not answer: %s", e.what()));
    }
}

bool App::ServerIsOurNode(const std::string& datadir, std::string& err)
{
    std::string cookie;
    {
        std::ifstream f(fs::PathFromString(datadir) / "backbone-app.cookie");
        std::getline(f, cookie);
    }
    const size_t colon{cookie.find(':')};
    std::string secret{colon == std::string::npos ? std::string{} : cookie.substr(colon + 1)};
    memory_cleanse(cookie.data(), cookie.size());
    if (secret.empty()) {
        err = "The node's cookie (backbone-app.cookie in its data folder) cannot be read: is another program answering on its app port?";
        return false;
    }
    const std::string challenge{bt::appui::random_hex(32)};
    const std::string expected{bt::appui::server_proof(secret, challenge)};
    memory_cleanse(secret.data(), secret.size());
    UniValue p{UniValue::VARR};
    p.push_back(challenge);
    bool ok{false};
    try {
        const UniValue proof{NodeCall("appserverproof", p, 5000)};
        ok = proof.isStr() && bt::appui::timing_safe_equal(proof.get_str(), expected);
    } catch (const HostError&) {
        ok = false;
    }
    if (!ok) {
        err = "Another program answers on the node's app port (it cannot prove it is the node): the app does not talk to it.";
        LogInfo("app: the program on the app port is not our node; stopping");
    }
    return ok;
}

UniValue App::FileTokens(const UniValue& params, std::vector<std::string>& used)
{
    // A request naming a file the user chose in this app's chooser for it: a one-time token of the node for it
    // (webuichosenfile), never the path alone; a fresh one each time the request goes to the node (the node takes a
    // token when it looks at the request, and a request asked in the dialog goes twice). The choice itself is
    // forgotten when the request ends (NodeRequest), whatever its outcome: each file serves one request.
    UniValue tokens{UniValue::VARR};
    std::vector<std::string> paths;
    std::function<void(const UniValue&)> scan = [&](const UniValue& v) {
        if (v.isStr()) paths.push_back(v.get_str());
        else if (v.isArray() || v.isObject())
            for (const UniValue& x : v.getValues()) scan(x);
    };
    scan(params);
    for (const std::string& path : paths) {
        Chosen c;
        {
            LOCK(m_mutex);
            const auto it{m_chosen.find(path)};
            if (it == m_chosen.end() || SteadySeconds() - it->second.when > CHOSEN_FILE_S) continue;
            c = it->second;
        }
        if (std::find(used.begin(), used.end(), path) == used.end()) used.push_back(path);
        UniValue p{UniValue::VARR};
        p.push_back(path);
        p.push_back(c.write);
        p.push_back(c.overwrite);
        tokens.push_back(NodeCall("webuichosenfile", p));
    }
    return tokens;
}

Window::ConfirmDialog App::Question(const UniValue& d)
{
    Window::ConfirmDialog dialog;
    dialog.title = Str(d, "title");
    dialog.message = Str(d, "message");
    dialog.accept = Str(d, "accept");
    dialog.cancel = Str(d, "cancel");
    dialog.details = Str(d, "details");
    dialog.level = LevelOf(Str(d, "level"));
    dialog.passphrase = false; // the LayerTwo wallets have no passphrase
    if (const UniValue& rows{Param(d, "rows")}; rows.isArray()) {
        for (const UniValue& r : rows.getValues()) {
            if (r.isObject()) dialog.rows.push_back({Str(r, "label"), Str(r, "value"), RowKind(Str(r, "kind"))});
        }
    }
    dialog.kind = "node:" + (Str(d, "kind").empty() ? dialog.title : Str(d, "kind"));
    return dialog;
}

UniValue App::NodeRequest(const std::string& kind, const std::string& method, const UniValue& params, const std::string& wallet)
{
    UniValue request{UniValue::VOBJ};
    request.pushKV("kind", kind);
    request.pushKV("method", method);
    request.pushKV("params", params);
    if (!wallet.empty()) request.pushKV("wallet", wallet);
    // The files this request uses, forgotten when it ends (answered, declined or failed).
    std::vector<std::string> files;
    struct Forget {
        App& app;
        std::vector<std::string>& paths;
        ~Forget()
        {
            LOCK(app.m_mutex);
            for (const std::string& path : paths) app.m_chosen.erase(path);
        }
    } forget{*this, files};
    auto send = [&](const std::string& token) {
        UniValue p{UniValue::VARR};
        p.push_back(request);
        p.push_back(token);
        p.push_back("");
        p.push_back(kind == "host" ? FileTokens(params, files) : UniValue{UniValue::VARR});
        return NodeCall("webuihost", p, REQUEST_TIMEOUT_MS);
    };
    UniValue answer{send("")};
    if (const UniValue& confirm{Param(answer, "confirm")}; confirm.isObject()) {
        // The node's question, in the app's trusted dialog: the page can neither draw nor answer it.
        const Window::ConfirmDialog question{Question(confirm)};
        if (!AskHost(question).accepted) throw HostError(webui::BRIDGE_CANCELLED, "Cancelled");
        answer = send(Str(confirm, "token"));
        if (Param(answer, "confirm").isObject()) throw HostError(webui::BRIDGE_INVALID_REQUEST, "A second question in one request: run them one at a time");
    }
    return Param(answer, "result");
}

bool App::StartNode(std::string& err)
{
    const NodeSettings s{Settings()};
    const std::string program{m_args.get("node", program_dir() + "/" + (m_spec.program.empty() ? m_spec.stem : m_spec.program)
#ifdef _WIN32
                                                   + ".exe"
#endif
                                                   )};
    if (!fs::exists(fs::PathFromString(program))) {
        err = strprintf("The node of %s is not where the app looks for it: %s", m_spec.name, program);
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(fs::PathFromString(s.datadir), ec);
    // The app's own user at the node, with a password of this run, and only what the pages use.
    const std::string user{m_spec.stem + "-app"};
    const std::string password{bt::appui::random_hex(32)};
    const bt::appui::RpcAuth auth{bt::appui::make_rpcauth(user, password)};
    std::string whitelist{user + ":"};
    for (size_t i{0}; i < m_spec.whitelist.size(); ++i) whitelist += (i ? "," : "") + m_spec.whitelist[i];
    const uint16_t app_port{FreePort(m_args.has("app-rpc-port") ? static_cast<uint16_t>(ToIntegral<uint16_t>(m_args.get("app-rpc-port")).value_or(0)) : static_cast<uint16_t>(s.rpc_port + 100))};
    if (!app_port) {
        err = "No free port for the node's app RPC";
        return false;
    }
    std::vector<std::string> args{"--datadir", s.datadir, "--network", s.network, m_spec.mainchain_url_flag, s.mainchain_url};
    // The LayerTwo RPC on loopback, as LayerTwo's node takes it (the public and the private server on one port).
    if (m_spec.address_flags == "host-port") {
        args.insert(args.end(), {"--rpc-host", "127.0.0.1", "--rpc-port", strprintf("%u", s.rpc_port), "--private-rpc-host", "127.0.0.1",
                                 "--private-rpc-port", strprintf("%u", s.rpc_port)});
    } else {
        args.insert(args.end(), {"--rpc-addr", strprintf("127.0.0.1:%u", s.rpc_port), "--private-rpc-addr", strprintf("127.0.0.1:%u", s.rpc_port)});
    }
    args.insert(args.end(), {"--net-addr", strprintf("0.0.0.0:%u", s.p2p_port), "--headless", "--log-level", "INFO",
                             "--bt-app-rpc-port", strprintf("%u", app_port), "--bt-app-rpcauth", bt::appui::format_rpcauth(auth),
                             "--bt-app-rpcwhitelist", whitelist, "--bt-app-rpcwhitelistdefault=0", "--bt-app-chooser", user, "--bt-app-exit-with-stdin"});
    for (const auto& p : s.peers) {
        args.push_back("--add-peer");
        args.push_back(p);
    }
    for (const auto& a : m_args.node_args) args.push_back(a);
    const std::string log{s.datadir + "/backbone-app-node.log"};
    {
        LOCK(m_mutex);
        m_node_log = log;
    }
    m_child = std::make_unique<Child>();
    LogInfo("app: starting %s (network %s, mainchain %s, data %s, app RPC 127.0.0.1:%u)", program, s.network, s.mainchain_url, s.datadir, app_port);
    if (!m_child->start(program, args, log, err)) return false;
    {
        LOCK(m_mutex);
        m_node_port = app_port;
        m_user = user;
        m_password = password;
    }
    // Ready when its app RPC answers (it starts after the store and the wallet are open).
    const auto deadline{std::chrono::steady_clock::now() + 180s};
    while (std::chrono::steady_clock::now() < deadline) {
        if (!m_child->running()) {
            err = strprintf("The node of %s stopped as it started (status %d).", m_spec.name, m_child->exit_status().value_or(-1));
            return false;
        }
        if (WITH_LOCK(m_mutex, return m_shutdown_requested)) return false;
        try {
            UniValue info{NodeCall("webuihost", [] {
                UniValue p{UniValue::VARR};
                UniValue r{UniValue::VOBJ};
                r.pushKV("kind", "host");
                r.pushKV("method", "app.info");
                r.pushKV("params", UniValue{UniValue::VOBJ});
                p.push_back(r);
                return p;
            }(), 5000)};
            // That it is our node that answers on that port, not a program that took the port first: it proves it holds
            // the cookie of the node's data folder (owner-only), which only the node writes.
            if (!ServerIsOurNode(s.datadir, err)) return false;
            LOCK(m_mutex);
            m_node_info = Param(info, "result");
            m_node_ready = true;
            return true;
        } catch (const HostError&) {
            std::this_thread::sleep_for(250ms);
        }
    }
    err = "The node did not answer in time";
    return false;
}

void App::StopNode()
{
    {
        LOCK(m_mutex);
        m_node_ready = false;
        ++m_generation;
    }
    if (m_events_thread.joinable()) m_events_thread.join();
    if (m_child) {
        // Asked to stop (it flushes its store); its input closes too (it stops if the call did not reach it).
        try {
            NodeCall("stop", UniValue{UniValue::VARR}, 5000);
        } catch (const HostError&) {
        }
        m_child->stop(60'000);
        m_child.reset();
    }
    LOCK(m_mutex);
    m_node_port = 0;
    memory_cleanse(m_password.data(), m_password.size());
    m_password.clear();
}

void App::EventsThread(uint64_t generation)
{
    util::ThreadRename("app-events");
    uint64_t since{0};
    while (true) {
        {
            LOCK(m_mutex);
            if (m_shutdown_requested || m_generation != generation) return;
        }
        try {
            UniValue p{UniValue::VARR};
            p.push_back(since);
            p.push_back(1500);
            const UniValue r{NodeCall("webuievents", p, 10'000)};
            since = static_cast<uint64_t>(Param(r, "seq").isNum() ? Param(r, "seq").getInt<int64_t>() : 0);
            const bool ready{WITH_LOCK(m_mutex, return m_page_ready)};
            if (!ready) continue;
            if (const UniValue& batches{Param(r, "batches")}; batches.isArray()) {
                for (const UniValue& b : batches.getValues()) {
                    if (!b.isStr()) continue;
                    // A batch as text the page parses (data, never code), as the template's events.
                    m_window->Post([this, text = b.get_str()] { DeliverEvents(text); });
                }
            }
        } catch (const HostError&) {
            std::this_thread::sleep_for(500ms);
        }
    }
}

void App::NodeThread()
{
    util::ThreadRename("app-node");
    // The node is started, watched and stopped by this one thread (its death signal is tied to the thread that made it).
    while (true) {
        SetPhase("init");
        {
            UniValue m{UniValue::VOBJ};
            m.pushKV("message", strprintf("Starting the node of %s…", m_spec.name));
            m_hub->Emit("init.message", m);
        }
        std::string err;
        if (!StartNode(err)) {
            if (WITH_LOCK(m_mutex, return m_shutdown_requested)) break;
            std::vector<std::string> details;
            if (!m_node_log.empty()) {
                std::istringstream lines{Tail(m_node_log, 12)};
                for (std::string l; std::getline(lines, l);) details.push_back(l);
            }
            Fatal(strprintf("%s could not start", m_spec.name), err, details);
            StopNode();
            WAIT_LOCK(m_mutex, lock);
            m_cv.wait(lock, [&] { return m_shutdown_requested || m_restart_node; });
            if (m_shutdown_requested) break;
            m_restart_node = false;
            continue;
        }
        UpdateTitle();
        SetPhase("ready");
        const uint64_t generation{WITH_LOCK(m_mutex, return m_generation)};
        m_events_thread = std::thread([this, generation] { EventsThread(generation); });
        bool crashed{false};
        {
            WAIT_LOCK(m_mutex, lock);
            while (!m_shutdown_requested && !m_restart_node) {
                m_cv.wait_for(lock, 500ms);
                if (m_child && !m_child->running()) {
                    crashed = true;
                    break;
                }
            }
        }
        if (crashed) {
            std::vector<std::string> details;
            std::istringstream lines{Tail(m_node_log, 12)};
            for (std::string l; std::getline(lines, l);) details.push_back(l);
            Fatal(strprintf("The node of %s stopped", m_spec.name), strprintf("It ended with status %d. Its log is %s.", m_child->exit_status().value_or(-1), m_node_log), details);
            StopNode();
            WAIT_LOCK(m_mutex, lock);
            m_cv.wait(lock, [&] { return m_shutdown_requested || m_restart_node; });
            if (m_shutdown_requested) break;
            m_restart_node = false;
            continue;
        }
        if (WITH_LOCK(m_mutex, return m_shutdown_requested)) break;
        // A restart (the settings of the node changed).
        StopNode();
        LOCK(m_mutex);
        m_restart_node = false;
    }
    SetPhase("shutdown");
    m_hub->Flush();
    StopNode();
    m_bridge->Stop();
    if (m_instance) m_instance->Stop();
    m_window->Quit();
}

// ------------------------------------------------------------------ the window
std::string App::Network()
{
    LOCK(m_mutex);
    return m_node_info.isObject() && Param(m_node_info, "network").isStr() ? Param(m_node_info, "network").get_str() : std::string{};
}

UniValue App::NetworkPrefs()
{
    std::string network{Network()};
    if (network.empty()) network = Settings().network;
    return m_config->Prefs(network);
}

bool App::PrefFlag(const std::string& key, bool fallback)
{
    const UniValue prefs{NetworkPrefs()};
    const UniValue& value{prefs.find_value(key)};
    return value.isBool() ? value.get_bool() : fallback;
}

UniValue App::Info()
{
    UniValue info{UniValue::VOBJ};
    UniValue node;
    std::string language;
    {
        LOCK(m_mutex);
        node = m_node_info;
        language = m_language;
    }
    const NodeSettings s{Settings()};
    // The node's own facts first (its network, its chain), then what the app says of itself.
    if (node.isObject()) {
        for (size_t i{0}; i < node.size(); ++i) info.pushKV(node.getKeys()[i], node.getValues()[i]);
    }
    auto set = [&](const std::string& k, const UniValue& v) {
        if (info.exists(k)) {
            UniValue copy{UniValue::VOBJ};
            for (size_t i{0}; i < info.size(); ++i) copy.pushKV(info.getKeys()[i], info.getKeys()[i] == k ? v : info.getValues()[i]);
            info = std::move(copy);
        } else {
            info.pushKV(k, v);
        }
    };
    set("name", m_spec.name);
    set("version", m_spec.version);
    set("bugreport", m_spec.bugreport);
    if (!info.exists("network")) set("network", s.network);
    set("networkLabel", NetworkLabel(Str(info, "network")));
    set("datadir", s.datadir);
    set("walletEnabled", true);
    set("devtools", m_devtools);
    set("language", language);
    set("test", !m_test_script.empty());
#ifdef _WIN32
    set("platform", "windows");
#elif defined(__APPLE__)
    set("platform", "mac");
#else
    set("platform", "linux");
#endif
    set("embedded", false);
    set("remote", false);
    if (!info.exists("blockSpacing")) set("blockSpacing", 600);
    return info;
}

void App::SetPhase(const std::string& phase, UniValue data)
{
    {
        LOCK(m_mutex);
        m_phase = phase;
        m_phase_data = data;
    }
    UniValue e{UniValue::VOBJ};
    e.pushKV("phase", phase);
    e.pushKV("data", data);
    m_hub->Emit("phase", e);
}

void App::Fatal(const std::string& title, const std::string& message, const std::vector<std::string>& details)
{
    LogError("app: %s: %s", title, message);
    UniValue data{UniValue::VOBJ};
    data.pushKV("title", title);
    data.pushKV("message", message);
    UniValue d{UniValue::VARR};
    for (const auto& x : details) d.push_back(webui::ValidUtf8(x));
    data.pushKV("details", d);
    SetPhase("fatal", data);
}

void App::DeliverEvents(const std::string& batch_json)
{
    m_window->Eval("window.__chainsEvents && window.__chainsEvents(" + UniValue{webui::ValidUtf8(batch_json)}.write() + ")");
}

void App::UpdateTitle()
{
    std::string title{m_spec.name};
    {
        LOCK(m_mutex);
        if (!m_wallet_title.empty()) title += " - " + m_wallet_title;
    }
    if (const std::string label{NetworkLabel(Network())}; !label.empty()) title += " [" + label + "]";
    m_window->Post([this, title] { m_window->SetTitle(title); });
}

UniValue App::WindowState()
{
    const webui::TitleBarMode mode{webui::ParseTitleBarMode(m_config->Value("titleBar"))};
    const bool supported{m_window->SupportsFrameless()};
    const bool frameless{m_window->IsFrameless()};
    UniValue state{UniValue::VOBJ};
    state.pushKV("titleBar", std::string{webui::TitleBarModeName(mode)});
    state.pushKV("supported", supported);
    state.pushKV("frameless", frameless);
    state.pushKV("restartNeeded", supported && frameless != (mode == webui::TitleBarMode::INTEGRATED));
    state.pushKV("maximized", m_window->IsMaximized());
    return state;
}

void App::RequestShutdown()
{
    {
        LOCK(m_mutex);
        if (m_shutdown_requested) return;
        m_shutdown_requested = true;
    }
    LogInfo("app: shutdown requested");
    m_cv.notify_all();
}

void App::QuitRequested()
{
    m_window->Post([this] {
        const std::optional<std::string> guard{m_quit_guard.Message(SteadySeconds())};
        if (!guard || guard->empty()) {
            RequestShutdown();
            return;
        }
        const std::string message{*guard};
        // Work a quit would interrupt: asked first, in the trusted dialog, off the UI thread.
        std::thread([this, message] {
            Window::ConfirmDialog d;
            d.title = strprintf("Quit %s?", m_spec.name);
            d.message = message;
            d.accept = "Quit";
            d.kind = "quit";
            if (Confirm(d)) RequestShutdown();
        }).detach();
    });
}

void App::CloseRequested()
{
    if (PrefFlag("minimizeOnClose", false)) {
        m_window->Minimize();
        return;
    }
    QuitRequested();
}

std::optional<std::string> App::Choose(const Window::FileDialog& dialog)
{
    std::optional<std::string> chosen;
    bool from_test{false};
#ifdef WEBUI_TEST_HOOKS
    {
        LOCK(m_mutex);
        if (!m_test_files.empty()) {
            const UniValue answer{m_test_files.front()};
            m_test_files.pop_front();
            if (answer.isStr()) chosen = answer.get_str();
            from_test = true;
        }
    }
#endif
    if (!from_test) {
        auto done{std::make_shared<std::promise<std::optional<std::string>>>()};
        Window::FileDialog shown{dialog};
        shown.scheme = dialog::SchemeOf(NetworkPrefs());
        m_window->Post([this, dialog = std::move(shown), done] { m_window->ChooseFile(dialog, [done](std::optional<std::string> path) { done->set_value(std::move(path)); }); });
        auto future{done->get_future()};
        while (future.wait_for(200ms) != std::future_status::ready) {
            if (WITH_LOCK(m_mutex, return m_shutdown_requested)) return std::nullopt;
        }
        chosen = future.get();
    }
    if (!chosen) return std::nullopt;
    if (!dialog.folder) {
        std::error_code ec;
        const bool existed{std::filesystem::exists(fs::PathFromString(*chosen), ec)};
        LOCK(m_mutex);
        (dialog.save ? m_writable : m_readable)[*chosen] = SteadySeconds();
        // For the node (a phrase saved to a file): a file there already only because the chooser asked to replace it.
        m_chosen[*chosen] = Chosen{SteadySeconds(), dialog.save, dialog.save && existed};
    }
    return chosen;
}

bool App::TakeChosenFile(const std::string& path, bool write)
{
    LOCK(m_mutex);
    auto& grants{write ? m_writable : m_readable};
    const auto it{grants.find(path)};
    if (it == grants.end()) return false;
    const bool fresh{SteadySeconds() - it->second <= CHOSEN_FILE_S};
    grants.erase(it);
    return fresh;
}

dialog::Request App::DialogRequest(const Window::ConfirmDialog& confirm)
{
    dialog::Request request;
    request.title = confirm.title;
    request.message = confirm.message;
    request.details = confirm.details;
    request.accept = confirm.accept;
    if (!confirm.cancel.empty()) request.cancel = confirm.cancel;
    request.level = confirm.level;
    request.rows = confirm.rows;
    request.passphrase = confirm.passphrase;
    request.passphrase_label = confirm.passphrase_label;
    request.error = confirm.error;
    std::string language;
    {
        LOCK(m_mutex);
        language = m_language;
    }
    bool rtl{false};
    if (auto catalog{webui::LoadCatalog(webui::BuiltinAssets(), language)}) rtl = catalog->find_value("rtl").isTrue();
    UniValue prefs{UniValue::VOBJ};
    try {
        prefs = NetworkPrefs();
    } catch (const std::exception&) {
    }
    request.look = dialog::Look(prefs, rtl, language);
    return request;
}

dialog::Answer App::AskHost(const Window::ConfirmDialog& confirm)
{
    // The template's rules (App::AskHost, audit 7 #8): every fact shown, none cut; one question of a kind at a time, a
    // few at once; a kind declined again and again is refused for the session; withdrawn when the page cancels.
    if (confirm.rows.size() > dialog::MAX_ROWS) throw HostError(webui::BRIDGE_INVALID_REQUEST, "Too much for one question: make it in parts");
    const std::string kind{confirm.kind.empty() ? confirm.title : confirm.kind};
    Window::ConfirmDialog shown{confirm};
    {
        LOCK(m_mutex);
        if (m_shutdown_requested) return {};
        QuestionKind& k{m_question_kinds[kind]};
        const int64_t now{SteadySeconds()};
        while (!k.declined.empty() && k.declined.front() < now - DECLINED_WINDOW_S) k.declined.pop_front();
        if (k.refused) return {};
        if (k.waiting || m_questions_waiting >= MAX_QUESTIONS_WAITING) return {};
        k.waiting = true;
        ++m_questions_waiting;
        if (k.declined.size() + 1 >= MAX_DECLINED) {
            shown.message = strprintf("This was asked and declined %u times in the last minute. Decline it once more and the app stops asking it until it starts again.\n\n%s",
                                      k.declined.size(), shown.message);
        }
    }
    ++m_host_questions;
    const auto answered{[&](bool accepted, bool counted) {
        --m_host_questions;
        LOCK(m_mutex);
        QuestionKind& k{m_question_kinds[kind]};
        k.waiting = false;
        --m_questions_waiting;
        if (!counted) return;
        if (accepted) {
            k.declined.clear();
        } else {
            k.declined.push_back(SteadySeconds());
            if (k.declined.size() >= MAX_DECLINED) k.refused = true;
        }
    }};
    dialog::Request request{DialogRequest(shown)};
#ifdef WEBUI_TEST_HOOKS
    {
        std::optional<TestConfirm> answer;
        bool unexpected{false};
        {
            LOCK(m_mutex);
            if (!m_test_confirms.empty()) {
                answer = m_test_confirms.front();
                m_test_confirms.pop_front();
            } else if (m_test_confirm_always) {
                answer = m_test_confirm_always;
            } else if (!m_test_script.empty()) {
                unexpected = true;
            }
            Window::ConfirmDialog recorded{shown};
            // A backup phrase is recorded as shown, never its words (the test scripts read what was asked).
            if (kind.find("phrase") != std::string::npos) recorded.details = "(the words, not recorded)";
            if (answer || unexpected) m_test_confirmed.push_back(recorded);
        }
        if (unexpected) {
            answered(false, false);
            return {};
        }
        if (answer) {
            if (*answer == TestConfirm::YES || *answer == TestConfirm::NO) {
                dialog::Answer out;
                out.accepted = *answer == TestConfirm::YES;
                answered(out.accepted, false);
                return out;
            }
            request.test_fallback = *answer == TestConfirm::FALLBACK;
        }
    }
#endif
    request.withdrawn = std::make_shared<std::atomic<bool>>(false);
    auto done{std::make_shared<std::promise<dialog::Answer>>()};
    m_window->Post([this, request, done] { m_window->Ask(request, [done](dialog::Answer answer) { done->set_value(std::move(answer)); }); });
    auto future{done->get_future()};
    while (future.wait_for(200ms) != std::future_status::ready) {
        const bool gone{Bridge::CurrentRequestDone() || WITH_LOCK(m_mutex, return m_shutdown_requested)};
        if (gone) {
            request.withdrawn->store(true);
            answered(false, false);
            return {};
        }
    }
    dialog::Answer answer{future.get()};
    answered(answer.accepted, true);
    return answer;
}

void App::RegisterHostMethods()
{
    Bridge& b{*m_bridge};
    b.RegisterHost("app.hello", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        UniValue dialogs{UniValue::VARR};
        bool first_hello;
        {
            LOCK(m_mutex);
            out.pushKV("phase", m_phase);
            out.pushKV("phaseData", m_phase_data);
            for (const auto& [id, d] : m_dialogs) dialogs.push_back(d);
            first_hello = !m_page_ready;
            m_page_ready = true;
        }
        out.pushKV("dialogs", std::move(dialogs));
        out.pushKV("info", Info());
        m_hub->SetReady(true);
#ifdef WEBUI_TEST_HOOKS
        if (first_hello && !m_test_script.empty()) {
            std::ifstream file{fs::PathFromString(m_test_script)};
            std::stringstream script;
            script << file.rdbuf();
            const std::string code{script.str() + "\n;window.__chainsTestScriptRan = true;\n"};
            const std::string check{"if (!window.__chainsTestScriptRan) {"
                                    " const say = (method, params) => window.__chains({ kind: 'host', method, params });"
                                    " say('test.log', { message: 'SCRIPT_ERROR the test script did not run: it does not parse, or throws at its start' })"
                                    ".finally(() => say('test.exit', { code: 1 })); }"};
            m_window->PostDelayed(100ms, [this, code] { m_window->Eval(code); });
            m_window->PostDelayed(3s, [this, check] { m_window->Eval(check); });
        }
#else
        (void)first_hello;
#endif
        return out;
    });
    b.RegisterHost("app.info", [this](const UniValue&) { return Info(); });
    b.RegisterHost("app.locale", [this](const UniValue&) {
        std::string language;
        {
            LOCK(m_mutex);
            language = m_language;
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("code", language);
        if (auto catalog{webui::LoadCatalog(webui::BuiltinAssets(), language)}) {
            out.pushKV("name", catalog->find_value("name"));
            out.pushKV("rtl", catalog->find_value("rtl"));
            out.pushKV("messages", catalog->find_value("gui"));
        } else {
            out.pushKV("name", "English");
            out.pushKV("rtl", false);
            out.pushKV("messages", UniValue{UniValue::VOBJ});
        }
        return out;
    });
    b.RegisterHost("app.languages", [](const UniValue&) { return webui::Languages(webui::BuiltinAssets()); });
    b.RegisterHost("app.updateLanguage", [this](const UniValue&) {
        const UniValue lang{m_config->Value("language")};
        const std::string language{webui::ChooseLanguage(webui::BuiltinAssets(), m_args.has("lang") ? m_args.get("lang") : lang.isStr() ? lang.get_str() : std::string{})};
        bool changed;
        {
            LOCK(m_mutex);
            changed = language != m_language;
            m_language = language;
        }
        webui::SetCoreCatalog(webui::LoadCatalog(webui::BuiltinAssets(), language));
        if (changed) {
            UniValue e{UniValue::VOBJ};
            e.pushKV("language", language);
            m_hub->Emit("language", e);
        }
        return UniValue{language};
    });
    b.RegisterHost("app.quit", [this](const UniValue&) {
        QuitRequested();
        return UniValue{true};
    });
    b.RegisterHost("app.quitGuard", [this](const UniValue& params) {
        const UniValue message{Param(params, "message")};
        if (!message.isNull() && !message.isStr()) throw HostError(webui::BRIDGE_INVALID_REQUEST, "message must be text or null");
        m_quit_guard.Set(message.isStr() ? message.get_str() : std::string{}, SteadySeconds());
        return UniValue{true};
    });
    b.RegisterHost("app.restart", [this](const UniValue&) {
        // The node starts again with the settings saved (the app stays).
        LOCK(m_mutex);
        m_restart_node = true;
        m_cv.notify_all();
        return UniValue{true};
    });
    b.RegisterHost("app.help", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        out.pushKV("usage", strprintf("Usage: %s-app [options]", m_spec.stem));
        out.pushKV("options", strprintf("  --network=<%s>\n  --datadir=<dir>\n  --mainchain-grpc-url=<url>\n  --rpc-port=<port>  --p2p-port=<port>\n  --node=<path of the node>  --node-arg=<an argument of the node> (repeatable)\n  --lang=<code>  --webuiconfig=<file>",
                                        [&] {
                                            std::string s;
                                            for (const auto& n : m_spec.networks) s += (s.empty() ? "" : "|") + n;
                                            return s;
                                        }()));
        return out;
    });
    b.RegisterHost("app.about", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        out.pushKV("name", m_spec.name);
        out.pushKV("version", m_spec.version);
        out.pushKV("license", strprintf("%s by Backbone, a client of LayerTwo's %s sidechain: its rules, its network and its RPC are theirs.\nInspired by %s by LayerTwo Labs.\n\nDistributed under the MIT software license.", m_spec.name, m_spec.name, m_spec.name));
        return out;
    });
    b.RegisterHost("intro.space", [](const UniValue&) -> UniValue { throw HostError(webui::BRIDGE_METHOD_NOT_FOUND, "No intro in this app"); });
    b.RegisterHost("dialog.answer", [this](const UniValue& params) {
        const UniValue id{Param(params, "id")};
        if (!id.isNum()) throw HostError(webui::BRIDGE_INVALID_REQUEST, "id: the id of the dialog is needed");
        {
            LOCK(m_mutex);
            if (!m_dialogs.contains(id.getInt<int64_t>())) return UniValue{false};
            if (m_informs.erase(id.getInt<int64_t>())) {
                m_dialogs.erase(id.getInt<int64_t>());
                return UniValue{true};
            }
            m_answers[id.getInt<int64_t>()] = Param(params, "answer");
        }
        m_cv.notify_all();
        return UniValue{true};
    });
    b.RegisterHost("prefs.get", [this](const UniValue&) { return NetworkPrefs(); });
    b.RegisterHost("prefs.set", [this](const UniValue& params) {
        if (!params.isObject()) throw HostError(webui::BRIDGE_INVALID_REQUEST, "prefs.set takes an object of the preferences to change");
        std::string network{Network()};
        if (network.empty()) network = Settings().network;
        try {
            return m_config->UpdatePrefs(network, params);
        } catch (const std::runtime_error& e) {
            throw HostError(e.what());
        }
    });
    const auto RefuseWhileQuestion{[this] {
        if (m_host_questions.load() > 0) throw HostError(webui::BRIDGE_CANCELLED, "A question of the app is open");
    }};
    b.RegisterHost("node.setNetworkActive", [](const UniValue&) -> UniValue { throw HostError("This node keeps its network on"); });
    b.RegisterHost("node.getSetting", [](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        out.pushKV("value", UniValue{});
        out.pushKV("ignored", false);
        return out;
    });
    b.RegisterHost("node.paths", [this](const UniValue&) {
        const NodeSettings s{Settings()};
        UniValue out{UniValue::VOBJ};
        out.pushKV("datadir", s.datadir);
        out.pushKV("blocksdir", s.datadir);
        out.pushKV("debuglog", m_node_log);
        out.pushKV("config", fs::PathToString(m_config->Path()));
        return out;
    });
    b.RegisterHost("window.setWallet", [this](const UniValue& params) {
        const UniValue name{Param(params, "name")};
        {
            LOCK(m_mutex);
            m_wallet_title = name.isStr() ? name.get_str() : "";
        }
        UpdateTitle();
        return UniValue{true};
    });
    b.RegisterHost("window.minimize", [this, RefuseWhileQuestion](const UniValue&) {
        RefuseWhileQuestion();
        m_window->Minimize();
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.state", [this](const UniValue&) { return WindowState(); }, Bridge::Thread::UI);
    b.RegisterHost("window.isMaximized", [this](const UniValue&) { return UniValue{m_window->IsMaximized()}; }, Bridge::Thread::UI);
    b.RegisterHost("window.toggleMaximize", [this, RefuseWhileQuestion](const UniValue&) {
        RefuseWhileQuestion();
        m_window->ToggleMaximize();
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.close", [this, RefuseWhileQuestion](const UniValue&) {
        RefuseWhileQuestion();
        CloseRequested();
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.startMove", [this, RefuseWhileQuestion](const UniValue&) {
        RefuseWhileQuestion();
        m_window->StartMove();
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.startResize", [this, RefuseWhileQuestion](const UniValue& params) {
        RefuseWhileQuestion();
        const auto edge{webui::ParseWindowEdge(ParamStr(params, "edge"))};
        if (!edge) throw HostError(webui::BRIDGE_INVALID_REQUEST, "edge: top, bottom, left, right, top-left, top-right, bottom-left or bottom-right");
        m_window->StartResize(*edge);
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.setTitleBar", [this](const UniValue& params) {
        const UniValue mode{Param(params, "mode")};
        if (!mode.isStr() || (mode.get_str() != "integrated" && mode.get_str() != "system")) throw HostError(webui::BRIDGE_INVALID_REQUEST, "mode: integrated or system");
        m_config->SetValue("titleBar", mode);
        if (m_window->SupportsFrameless()) m_window->SetFrameless(webui::ParseTitleBarMode(mode) == webui::TitleBarMode::INTEGRATED);
        return WindowState();
    }, Bridge::Thread::UI);
    b.RegisterHost("file.choose", [this](const UniValue& params) {
        Window::FileDialog dialog;
        dialog.save = Param(params, "save").isTrue();
        dialog.folder = Param(params, "folder").isTrue();
        dialog.title = Param(params, "title").isStr() ? Param(params, "title").get_str() : (dialog.save ? "Save" : "Open");
        if (Param(params, "name").isStr()) dialog.suggested_name = Param(params, "name").get_str();
        if (const UniValue& filters{Param(params, "filters")}; filters.isArray()) {
            for (const UniValue& f : filters.getValues()) {
                std::vector<std::string> patterns;
                if (f.find_value("patterns").isArray()) {
                    for (const UniValue& pattern : f.find_value("patterns").getValues()) {
                        if (pattern.isStr()) patterns.push_back(pattern.get_str());
                    }
                }
                dialog.filters.emplace_back(f.find_value("name").isStr() ? f.find_value("name").get_str() : "Files", std::move(patterns));
            }
        }
        const auto chosen{Choose(dialog)};
        return chosen ? UniValue{*chosen} : UniValue{};
    }, Bridge::Thread::LONG);
    b.RegisterHost("file.read", [this](const UniValue& params) {
        const std::string path{ParamStr(params, "path")};
        if (!TakeChosenFile(path, false)) throw HostError(webui::BRIDGE_INVALID_REQUEST, "Only a file chosen by the user can be read, once, soon after");
        std::ifstream file{fs::PathFromString(path), std::ios::binary};
        if (!file) throw HostError(strprintf("Cannot open %s", path));
        file.seekg(0, std::ios::end);
        const auto size{static_cast<int64_t>(file.tellg())};
        if (size < 0 || size >= 100'000'000) throw HostError("The file must be smaller than 100 MB");
        file.seekg(0);
        std::string data(static_cast<size_t>(size), '\0');
        file.read(data.data(), size);
        UniValue out{UniValue::VOBJ};
        out.pushKV("path", path);
        out.pushKV("size", size);
        out.pushKV("base64", EncodeBase64(data));
        return out;
    });
    b.RegisterHost("file.write", [this](const UniValue& params) {
        const std::string path{ParamStr(params, "path")};
        if (!TakeChosenFile(path, true)) throw HostError(webui::BRIDGE_INVALID_REQUEST, "Only a file chosen by the user can be written, once, soon after");
        std::string data;
        if (Param(params, "base64").isStr()) {
            auto bytes{DecodeBase64(Param(params, "base64").get_str())};
            if (!bytes) throw HostError(webui::BRIDGE_INVALID_REQUEST, "base64: not base64");
            data.assign(bytes->begin(), bytes->end());
        } else {
            data = ParamStr(params, "text");
        }
        std::ofstream file{fs::PathFromString(path), std::ios::binary | std::ios::trunc};
        if (!file || !file.write(data.data(), data.size()) || !(file.flush())) throw HostError(strprintf("There was an error trying to save to %s.", path));
        return UniValue{path};
    });
    b.RegisterHost("clipboard.write", [this](const UniValue& params) {
        const std::string text{ParamStr(params, "text")};
        if (text.size() > MAX_CLIPBOARD_BYTES) throw HostError(webui::BRIDGE_INVALID_REQUEST, "The text is too long for the clipboard");
        const auto input{m_window->LastUserInput()};
        {
            LOCK(m_mutex);
            if (!input || input->age_ms > CLIPBOARD_WRITE_MS || input->serial == m_clipboard_write_gesture) {
                throw HostError(webui::BRIDGE_CANCELLED, "The clipboard is written only right after you ask for it, with a click or a key");
            }
            m_clipboard_write_gesture = input->serial;
        }
        m_window->SetClipboard(text);
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("clipboard.read", [this](const UniValue&) {
        const auto input{m_window->LastUserInput()};
        {
            LOCK(m_mutex);
            const int64_t now{SteadySeconds()};
            while (!m_clipboard_reads.empty() && m_clipboard_reads.front() < now - 60) m_clipboard_reads.pop_front();
            if (!input || input->age_ms > CLIPBOARD_READ_MS || input->serial == m_clipboard_read_gesture || m_clipboard_reads.size() >= MAX_CLIPBOARD_READS) {
                throw HostError(webui::BRIDGE_CANCELLED, "The clipboard is read only right after you ask for it, with a click or a key");
            }
            m_clipboard_read_gesture = input->serial;
            m_clipboard_reads.push_back(now);
        }
        const auto text{m_window->GetClipboard()};
        return text ? UniValue{*text} : UniValue{};
    }, Bridge::Thread::UI);
    b.RegisterHost("app.open", [this](const UniValue& params) {
        const std::string what{ParamStr(params, "what")};
        std::string uri;
        if (what == "debuglog") {
            std::string log;
            {
                LOCK(m_mutex);
                log = m_node_log;
            }
            if (log.empty() || !fs::exists(fs::PathFromString(log))) throw HostError("There is no log of the node yet.");
            uri = "file://";
            std::string abs{fs::PathToString(fs::absolute(fs::PathFromString(log)))};
            std::string part;
            for (char c : abs + "/") {
                if (c == '/' || c == '\\') {
                    if (!part.empty()) uri += "/" + webui::PercentEncode(part);
                    part.clear();
                } else {
                    part += c;
                }
            }
        } else if (what == "url") {
            uri = ParamStr(params, "url");
            std::optional<Window::ConfirmDialog> d;
            try {
                d = webui::guard::OpenUrlConfirmation(uri);
            } catch (const std::invalid_argument& e) {
                throw HostError(webui::BRIDGE_INVALID_REQUEST, e.what());
            }
            if (d && !Confirm(*d)) return UniValue{false};
        } else {
            throw HostError(webui::BRIDGE_INVALID_REQUEST, "what: debuglog or url");
        }
        auto done{std::make_shared<std::promise<bool>>()};
        m_window->Post([this, uri, done] { done->set_value(m_window->OpenExternal(uri)); });
        auto future{done->get_future()};
        if (future.wait_for(10s) != std::future_status::ready) return UniValue{false};
        return UniValue{future.get()};
    }, Bridge::Thread::LONG);
    b.RegisterHost("app.notify", [this](const UniValue& params) {
        const std::string title{webui::guard::OneLine(ParamStr(params, "title"), 80)};
        const std::string body{Param(params, "body").isStr() ? dialog::Shown(Param(params, "body").get_str(), 300) : ""};
        const std::string level{Param(params, "level").isStr() ? Param(params, "level").get_str() : "info"};
        {
            LOCK(m_mutex);
            UniValue sent{UniValue::VOBJ};
            sent.pushKV("title", title);
            sent.pushKV("body", body);
            sent.pushKV("level", level);
            m_notifications.push_back(std::move(sent));
            if (m_notifications.size() > 100) m_notifications.erase(m_notifications.begin());
        }
        m_window->Post([this, title, body, level] { m_window->Notify(title, body, level); });
        return UniValue{true};
    });
    b.RegisterHost("dialog.prepare", [this](const UniValue&) {
        m_window->Post([this] { m_window->PrepareDialog(); });
        return UniValue{true};
    });
    b.RegisterHost("tray.state", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        out.pushKV("available", false);
        out.pushKV("visible", false);
        out.pushKV("windowVisible", m_window->IsVisible());
        return out;
    }, Bridge::Thread::UI);
    // The kit's Settings reads the language among the node's options: here the app keeps it (the node has none).
    const auto options_json{[this] {
        UniValue o{UniValue::VOBJ};
        const UniValue lang{m_config->Value("language")};
        o.pushKV("lang", lang.isStr() ? lang.get_str() : "");
        o.pushKV("restartRequired", false);
        o.pushKV("pending", 0);
        o.pushKV("ignored", UniValue{UniValue::VOBJ});
        return o;
    }};
    b.RegisterHost("options.get", [options_json](const UniValue&) { return options_json(); });
    b.RegisterHost("options.apply", [options_json](const UniValue&) { return options_json(); });
    b.RegisterHost("options.cancel", [options_json](const UniValue&) { return options_json(); });
    b.RegisterHost("options.set", [this, options_json](const UniValue& params) {
        const std::string name{ParamStr(params, "name")};
        if (name != "lang") throw HostError(webui::BRIDGE_INVALID_REQUEST, "This app has no such option");
        const UniValue& value{Param(params, "value")};
        if (!value.isStr() || value.get_str().size() > 16) throw HostError(webui::BRIDGE_INVALID_REQUEST, "lang: a language code");
        m_config->SetValue("language", value);
        return options_json();
    });
    // The settings of the node this app runs (Settings → Node): saved, used at its next start (app.restart).
    b.RegisterHost("appnode.get", [this](const UniValue&) { return SettingsJson(); });
    b.RegisterHost("appnode.set", [this](const UniValue& params) {
        if (!params.isObject()) throw HostError(webui::BRIDGE_INVALID_REQUEST, "appnode.set takes an object");
        UniValue saved{m_config->Value("node")};
        if (!saved.isObject()) saved = UniValue{UniValue::VOBJ};
        UniValue next{UniValue::VOBJ};
        for (size_t i{0}; i < saved.size(); ++i) {
            if (!params.exists(saved.getKeys()[i])) next.pushKV(saved.getKeys()[i], saved.getValues()[i]);
        }
        for (size_t i{0}; i < params.size(); ++i) {
            const std::string& k{params.getKeys()[i]};
            const UniValue& v{params.getValues()[i]};
            if (k == "network") {
                if (!v.isStr() || std::find(m_spec.networks.begin(), m_spec.networks.end(), v.get_str()) == m_spec.networks.end()) throw HostError(webui::BRIDGE_INVALID_REQUEST, "network: one of the node's networks");
            } else if (k == "mainchainUrl") {
                if (!v.isStr() || !(v.get_str().rfind("http://", 0) == 0 || v.get_str().rfind("https://", 0) == 0) || v.get_str().size() > 300 ||
                    v.get_str().find_first_of(" \t\r\n\"'") != std::string::npos) {
                    throw HostError(webui::BRIDGE_INVALID_REQUEST, "mainchainUrl: an http:// or https:// address of the mainchain's interface");
                }
            } else if (k == "datadir") {
                if (!v.isStr() || v.get_str().empty() || !fs::PathFromString(v.get_str()).is_absolute()) throw HostError(webui::BRIDGE_INVALID_REQUEST, "datadir: an absolute folder");
            } else if (k == "rpcPort" || k == "p2pPort") {
                if (!v.isNum() || v.getInt<int>() < 1024 || v.getInt<int>() > 65535) throw HostError(webui::BRIDGE_INVALID_REQUEST, k + ": a port from 1024 to 65535");
            } else if (k == "peers") {
                if (!v.isArray() || v.size() > 32) throw HostError(webui::BRIDGE_INVALID_REQUEST, "peers: at most 32 host:port");
                for (const UniValue& p : v.getValues()) {
                    if (!p.isStr() || p.get_str().size() > 255 || p.get_str().find_first_of(" \t\r\n") != std::string::npos) throw HostError(webui::BRIDGE_INVALID_REQUEST, "peers: host:port each");
                }
            } else {
                throw HostError(webui::BRIDGE_INVALID_REQUEST, "No setting " + k);
            }
            next.pushKV(k, v);
        }
        // What decides what the node believes of eCash (its mainchain interface: deposits, withdrawals, the wallet that pays
        // deposits) or where its keys live: on the user's word in the trusted dialog, never on the page's alone.
        const NodeSettings before{Settings()};
        Window::ConfirmDialog d;
        d.title = "Change the node's settings";
        d.message = "The node takes them when it starts again. The mainchain interface decides what this node believes of eCash: use only one you run or trust.";
        d.accept = "Change";
        d.kind = "appnode.set";
        auto row = [&](const std::string& key, const std::string& label, const std::string& was) {
            const UniValue& v{Param(next, key)};
            if (v.isStr() && v.get_str() != was) d.rows.push_back({label, v.get_str(), dialog::Row::Kind::TEXT});
        };
        row("network", "Network", before.network);
        row("mainchainUrl", "Mainchain interface", before.mainchain_url);
        row("datadir", "Data folder", before.datadir);
        if (!d.rows.empty() && !Confirm(d)) throw HostError(webui::BRIDGE_CANCELLED, "Cancelled");
        m_config->SetValue("node", next);
        return SettingsJson();
    }, Bridge::Thread::LONG);
    b.RegisterHost("appnode.chooseDatadir", [this](const UniValue&) {
        Window::FileDialog d;
        d.folder = true;
        d.title = "The node's data folder";
        d.start_dir = Settings().datadir;
        const auto chosen{Choose(d)};
        return chosen ? UniValue{*chosen} : UniValue{};
    }, Bridge::Thread::LONG);

    // The node's own: forwarded to its app RPC (webuihost), its questions asked here.
    std::set<std::string> forwarded{m_spec.node_host_methods.begin(), m_spec.node_host_methods.end()};
    forwarded.insert({"node.state", "rpc.confirmed", "rpc.confirmedMethods"});
    for (const std::string& name : forwarded) {
        const bool asks{name == "rpc.confirmed" || name.find(".seed.") != std::string::npos};
        b.RegisterHost(name, [this, name](const UniValue& params) { return NodeRequest("host", name, params, Str(params, "wallet")); },
                       asks ? Bridge::Thread::LONG : Bridge::Thread::WORKER);
    }
    m_bridge->SetRpc([this](const std::string& method, const UniValue& params, const std::string& uri) {
        std::string wallet;
        if (uri.rfind("/wallet/", 0) == 0) wallet = bt::appui::url_decode(uri.substr(8));
        try {
            return NodeRequest("rpc", method, params, wallet);
        } catch (const HostError& e) {
            UniValue err{UniValue::VOBJ};
            err.pushKV("code", e.code);
            err.pushKV("message", e.what());
            throw err;
        }
    });

#ifdef WEBUI_TEST_HOOKS
    if (!m_test_script.empty()) {
        b.RegisterHost("test.log", [](const UniValue& params) {
            const std::string message{params.isObject() && params.find_value("message").isStr() ? params.find_value("message").get_str() : params.write()};
            LogInfo("webui test: %s", message);
            tfm::format(std::cout, "webui test: %s\n", message);
            std::cout.flush();
            return UniValue{true};
        });
        b.RegisterHost("test.snapshot", [this](const UniValue& params) {
            const std::string name{ParamStr(params, "name")};
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
                throw HostError(webui::BRIDGE_INVALID_REQUEST, "name: letters, digits, '.', '_' and '-'");
            }
            if (m_test_shots.empty()) throw HostError("No --webuitestshots folder");
            const fs::path dir{fs::PathFromString(m_test_shots)};
            fs::create_directories(dir);
            const std::string path{fs::PathToString(dir / fs::PathFromString(name + ".png"))};
            auto done{std::make_shared<std::promise<bool>>()};
            m_window->Post([this, path, done] { m_window->Snapshot(path, [done](bool ok) { done->set_value(ok); }); });
            auto future{done->get_future()};
            if (future.wait_for(30s) != std::future_status::ready || !future.get()) throw HostError("The snapshot failed");
            return UniValue{path};
        }, Bridge::Thread::LONG);
        b.RegisterHost("test.fileAnswer", [this](const UniValue& params) {
            LOCK(m_mutex);
            m_test_files.push_back(Param(params, "path"));
            return UniValue{true};
        });
        b.RegisterHost("test.confirmAnswer", [this](const UniValue& params) {
            LOCK(m_mutex);
            if (Param(params, "clear").isTrue()) {
                m_test_confirms.clear();
                return UniValue{true};
            }
            if (Param(params, "always").isBool()) {
                if (Param(params, "always").get_bool()) m_test_confirm_always = TestConfirm::YES;
                else m_test_confirm_always.reset();
                return UniValue{true};
            }
            const UniValue& yes{Param(params, "yes")};
            const bool show{Param(params, "show").isTrue()};
            const bool fallback{Param(params, "fallback").isTrue()};
            if (!yes.isBool() && !show && !fallback) throw HostError(webui::BRIDGE_INVALID_REQUEST, "yes: true or false; or show: true; or fallback: true");
            m_test_confirms.push_back(fallback ? TestConfirm::FALLBACK : show ? TestConfirm::SHOW : yes.get_bool() ? TestConfirm::YES : TestConfirm::NO);
            return UniValue{true};
        });
        b.RegisterHost("test.confirmed", [this](const UniValue&) {
            LOCK(m_mutex);
            UniValue out{UniValue::VARR};
            for (const auto& d : m_test_confirmed) {
                UniValue entry{UniValue::VOBJ};
                entry.pushKV("title", d.title);
                entry.pushKV("message", d.message);
                entry.pushKV("details", d.details);
                entry.pushKV("accept", d.accept);
                entry.pushKV("kind", d.kind);
                entry.pushKV("level", std::string{dialog::LevelName(d.level)});
                UniValue rows{UniValue::VARR};
                for (const auto& row : d.rows) {
                    UniValue r{UniValue::VOBJ};
                    r.pushKV("label", row.label);
                    r.pushKV("value", row.value);
                    r.pushKV("kind", std::string{dialog::RowKindName(row.kind)});
                    rows.push_back(std::move(r));
                }
                entry.pushKV("rows", std::move(rows));
                out.push_back(std::move(entry));
            }
            return out;
        });
        b.RegisterHost("test.gesture", [this](const UniValue&) {
            m_window->NoteUserInput();
            return UniValue{true};
        }, Bridge::Thread::UI);
        b.RegisterHost("test.dialog", [this](const UniValue& params) {
            const std::string action{ParamStr(params, "action")};
            std::string arg;
            if (action == "eval") {
                arg = ParamStr(params, "script");
            } else if (action == "snapshot") {
                const std::string name{ParamStr(params, "name")};
                if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
                    throw HostError(webui::BRIDGE_INVALID_REQUEST, "name: letters, digits, '.', '_' and '-'");
                }
                if (m_test_shots.empty()) throw HostError("No --webuitestshots folder");
                const fs::path dir{fs::PathFromString(m_test_shots)};
                fs::create_directories(dir);
                arg = fs::PathToString(dir / fs::PathFromString(name + ".png"));
            } else if (action == "native") {
                arg = ParamStr(params, "arg");
            } else if (action != "kind") {
                throw HostError(webui::BRIDGE_INVALID_REQUEST, "action: kind, eval, snapshot or native");
            }
            auto done{std::make_shared<std::promise<std::optional<std::string>>>()};
            m_window->Post([this, action, arg, done] { m_window->TestDialog(action, arg, [done](std::optional<std::string> out) { done->set_value(std::move(out)); }); });
            auto future{done->get_future()};
            if (future.wait_for(30s) != std::future_status::ready) throw HostError("The dialog did not answer");
            const auto out{future.get()};
            if (!out) return UniValue{};
            UniValue value;
            if (!value.read(*out)) return UniValue{*out};
            if (action == "snapshot" && value.isTrue()) return UniValue{arg};
            return value;
        }, Bridge::Thread::LONG);
        b.RegisterHost("test.notifications", [this](const UniValue&) {
            LOCK(m_mutex);
            UniValue out{UniValue::VARR};
            for (const auto& n : m_notifications) out.push_back(n);
            return out;
        });
        // The test drives the node itself as a user of the node's whole RPC would (blocks, its L2L methods): only in a
        // build for the tests, only with the test script, never through the pages' policy.
        b.RegisterHost("test.node", [this](const UniValue& params) {
            UniValue p{Param(params, "params")};
            return NodeCall(ParamStr(params, "method"), p.isArray() ? p : UniValue{UniValue::VARR}, 120'000);
        }, Bridge::Thread::LONG);
        b.RegisterHost("test.exit", [this](const UniValue& params) {
            const UniValue code{Param(params, "code")};
            {
                LOCK(m_mutex);
                m_exit_code = code.isNum() ? code.getInt<int>() : EXIT_FAILURE;
            }
            RequestShutdown();
            return UniValue{true};
        });
    }
#endif
}

int App::Run()
{
    const std::string config_path{m_args.get("webuiconfig")};
    m_config = std::make_unique<webui::AppConfig>(config_path.empty() ? webui::AppConfig::DefaultPath() : fs::PathFromString(config_path));
    m_config->Load();
    {
        const UniValue lang{m_config->Value("language")};
        const std::string language{webui::ChooseLanguage(webui::BuiltinAssets(), m_args.has("lang") ? m_args.get("lang") : lang.isStr() ? lang.get_str() : std::string{})};
        LOCK(m_mutex);
        m_language = language;
    }
    webui::SetCoreCatalog(webui::LoadCatalog(webui::BuiltinAssets(), WITH_LOCK(m_mutex, return m_language)));

    // One app per data directory of the node: a second start shows the first one.
    const fs::path socket{webui::SingleInstance::SocketPath(fs::PathFromString(Settings().datadir))};
    if (webui::SingleInstance::SendToRunning(socket, {})) {
        tfm::format(std::cout, "%s runs already for this data directory: asked it to show itself.\n", m_spec.name);
        return EXIT_SUCCESS;
    }

    Window::Options options;
    options.title = m_spec.name;
    options.devtools = m_devtools;
    if (const UniValue geometry{m_config->Value("window")}; geometry.isObject()) {
        const UniValue& width{geometry.find_value("width")};
        const UniValue& height{geometry.find_value("height")};
        if (width.isNum() && height.isNum() && width.getInt<int>() >= 400 && height.getInt<int>() >= 300) {
            options.width = std::min(width.getInt<int>(), 10000);
            options.height = std::min(height.getInt<int>(), 10000);
        }
        options.maximized = geometry.find_value("maximized").isTrue();
    }
    options.frameless = webui::ParseTitleBarMode(m_config->Value("titleBar")) == webui::TitleBarMode::INTEGRATED;
    Window::Callbacks callbacks;
    callbacks.close_requested = [this] { CloseRequested(); };
    callbacks.maximized_changed = [this](bool maximized) {
        UniValue data{UniValue::VOBJ};
        data.pushKV("maximized", maximized);
        if (m_hub) m_hub->Emit("window", std::move(data), "window");
    };
    callbacks.session_ending = [this] { RequestShutdown(); };
    callbacks.call = [this](const std::string& id, const std::string& args) { m_bridge->Handle(id, args); };
    m_window = Window::Create(options, std::move(callbacks));

    m_hub = std::make_unique<EventHub>([this](const std::string& batch) { DeliverEvents(batch); },
                                       [this](std::chrono::milliseconds delay, std::function<void()> fn) {
                                           if (delay.count() == 0) m_window->Post(std::move(fn));
                                           else m_window->PostDelayed(delay, std::move(fn));
                                       });
    m_bridge = std::make_unique<Bridge>([this](const std::string& id, int status, const std::string& json) { m_window->Reply(id, status, json); },
                                        [this](std::function<void()> fn) { m_window->Post(std::move(fn)); });
    m_bridge->SetInternalError([this](const std::string& method, const std::string& what) {
        LogWarning("app: internal error in %s: %s", method, what);
        UniValue d{UniValue::VOBJ};
        d.pushKV("kind", "message");
        d.pushKV("level", "error");
        d.pushKV("title", "Internal error");
        d.pushKV("message", strprintf("An internal error occurred. %s will attempt to continue safely.", m_spec.name));
        UniValue details{UniValue::VARR};
        details.push_back(strprintf("%s: %s", method, what));
        details.push_back(m_spec.bugreport);
        d.pushKV("details", std::move(details));
        m_hub->Emit("dialog", std::move(d));
    });
    RegisterHostMethods();

    m_instance = std::make_unique<webui::SingleInstance>();
    if (!m_instance->Listen(socket, [this](const std::string&) { m_window->Post([this] { m_window->Present(); }); })) {
        LogWarning("app: other starts of the app cannot reach this one");
    }
    UpdateTitle();
    m_window->Post([this] { m_window->Show(m_args.has("min")); });
    m_node_thread = std::thread([this] { NodeThread(); });
    m_window->Run();

    RequestShutdown();
    if (const Window::Geometry geometry{m_window->GetGeometry()}; geometry.width > 0 && geometry.height > 0) {
        UniValue saved{UniValue::VOBJ};
        saved.pushKV("width", geometry.width);
        saved.pushKV("height", geometry.height);
        saved.pushKV("maximized", geometry.maximized);
        m_config->SetValue("window", saved);
    }
    if (m_node_thread.joinable()) m_node_thread.join();
    m_bridge.reset();
    m_window.reset();
    LOCK(m_mutex);
    return m_exit_code.value_or(EXIT_SUCCESS);
}

int Main(int argc, char** argv, const ChainSpec& spec)
{
    Args args;
    for (int i{1}; i < argc; ++i) {
        std::string a{argv[i]};
        if (a == "-h" || a == "--help") {
            std::printf("Usage: %s-app [options]\n\n  --network=<%s>\n  --datadir=<dir>             the node's data folder\n  --mainchain-grpc-url=<url>  the mainchain's interface (the enforcer's gRPC, or backboned -grpc)\n"
                        "  --rpc-port=<port>  --p2p-port=<port>  the node's LayerTwo RPC and P2P ports\n  --node=<path>               the node's program (default: %s next to the app)\n"
                        "  --node-arg=<arg>            an argument of the node (repeatable)\n  --lang=<code>  --min  --webuiconfig=<file>  --webuidevtools\n",
                        spec.stem.c_str(), [&] {
                            std::string s;
                            for (const auto& n : spec.networks) s += (s.empty() ? "" : "|") + n;
                            return s;
                        }().c_str(), spec.stem.c_str());
            return 0;
        }
        if (a == "-V" || a == "--version") {
            std::printf("%s-app %s\n", spec.stem.c_str(), spec.version.c_str());
            return 0;
        }
        while (!a.empty() && a.front() == '-') a.erase(0, 1);
        const size_t eq{a.find('=')};
        const std::string key{a.substr(0, eq)};
        std::string value{eq == std::string::npos ? std::string{} : a.substr(eq + 1)};
        static const std::set<std::string> with_value{"network", "datadir", "mainchain-grpc-url", "rpc-port", "p2p-port", "app-rpc-port", "node", "node-arg", "lang", "webuiconfig",
                                                      "webuitestscript", "webuitestshots"};
        if (with_value.contains(key) && eq == std::string::npos) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: a value is required for --%s\n", key.c_str());
                return 2;
            }
            value = argv[++i];
        }
        if (key == "node-arg") args.node_args.push_back(value);
        else if (with_value.contains(key) || key == "webuidevtools" || key == "min") args.values[key] = value;
        else {
            std::fprintf(stderr, "error: unknown option --%s (see --help)\n", key.c_str());
            return 2;
        }
    }
#ifndef WEBUI_TEST_HOOKS
    if (args.has("webuitestscript")) {
        std::fprintf(stderr, "error: this build has no test hooks\n");
        return 2;
    }
#endif
    bt::log::set_console_level(bt::log::Level::Info);
    App app{spec, std::move(args)};
    return app.Run();
}

} // namespace bt::desktop
