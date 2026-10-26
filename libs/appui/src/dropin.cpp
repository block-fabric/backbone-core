// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/appui/dropin.h>

#include <bt/fsperm.h>
#include <bt/log.h>
#include <bt/parse.h>

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <tuple>

namespace bt::appui {

namespace {
int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

// ------------------------------------------------------------------ options
bool take_option(AppOptions& o, int argc, char** argv, int& i, std::string& err)
{
    std::string a{argv[i]}, val;
    bool has_eq{false};
    if (const size_t eq{a.find('=')}; a.rfind("--", 0) == 0 && eq != std::string::npos) {
        val = a.substr(eq + 1);
        a = a.substr(0, eq);
        has_eq = true;
    }
    if (a.rfind("--bt-app-", 0) != 0) return false;
    auto value = [&](std::string& out) {
        if (has_eq) {
            out = val;
            return true;
        }
        if (i + 1 < argc) {
            out = argv[++i];
            return true;
        }
        err = "a value is required for '" + a + "'";
        return false;
    };
    std::string v;
    if (a == "--bt-app-rpc-port") {
        if (!value(v)) return true;
        auto p{parse_u64(v)};
        if (!p || *p == 0 || *p > 65535) {
            err = "invalid --bt-app-rpc-port " + v;
            return true;
        }
        o.enabled = true;
        o.port = static_cast<uint16_t>(*p);
    } else if (a == "--bt-app-rpcauth") {
        if (!value(v)) return true;
        if (!parse_rpcauth(v)) {
            err = "invalid --bt-app-rpcauth (user:salt$hash, as share/rpcauth makes)";
            return true;
        }
        o.rpcauth.push_back(v);
    } else if (a == "--bt-app-rpcwhitelist") {
        if (!value(v)) return true;
        o.whitelist.push_back(v);
    } else if (a == "--bt-app-rpcwhitelistdefault") {
        if (has_eq) o.whitelist_default = !(val == "0" || val == "false");
        else o.whitelist_default = true;
    } else if (a == "--bt-app-chooser") {
        if (!value(v)) return true;
        o.chooser = v;
    } else if (a == "--bt-app-filedir") {
        if (!value(v)) return true;
        o.file_dir = v;
    } else if (a == "--bt-app-exit-with-stdin") {
        o.exit_with_stdin = true;
    } else {
        err = "unknown option " + a;
    }
    return true;
}

std::string usage()
{
    return "Backbone app layer (our app and the unified wallet; the LayerTwo RPC above is not changed by it):\n"
           "      --bt-app-rpc-port <PORT>               Serve the app RPC on 127.0.0.1:<PORT> (off by default): our pages' host,\n"
           "                                             authenticated by a cookie (<DATADIR>/backbone-app.cookie) or --bt-app-rpcauth\n"
           "      --bt-app-rpcauth <USER:SALT$HASH>      A user of the app RPC (share/rpcauth form; repeatable)\n"
           "      --bt-app-rpcwhitelist <USER:M1,M2,...> What a user may call (repeatable: the intersection)\n"
           "      --bt-app-rpcwhitelistdefault[=0|1]     Users without a whitelist may call nothing (1) or everything (0)\n"
           "      --bt-app-chooser <USER>                The app's own user, the only one (with unrestricted ones) that grants chosen files\n"
           "      --bt-app-filedir <DIR>                 Grant only files of this folder\n"
           "      --bt-app-exit-with-stdin               Stop when standard input closes (the app that started the node went)\n";
}

ServerOptions server_options(const AppOptions& o, const std::string& datadir, std::string& err)
{
    ServerOptions s;
    s.port = o.port;
    s.cookie_file = datadir + "/backbone-app.cookie";
    for (const std::string& a : o.rpcauth) {
        auto parsed{parse_rpcauth(a)};
        if (!parsed) {
            err = "invalid --bt-app-rpcauth";
            return s;
        }
        s.auths.push_back(*parsed);
    }
    for (const std::string& w : o.whitelist) {
        if (!s.whitelist.add(w)) {
            err = "invalid --bt-app-rpcwhitelist " + w;
            return s;
        }
    }
    if (o.whitelist_default) s.whitelist.set_default(*o.whitelist_default);
    return s;
}

// ------------------------------------------------------------------ amounts
std::string format_sats(uint64_t sats)
{
    std::string frac{std::to_string(sats % 100'000'000)};
    frac.insert(0, 8 - frac.size(), '0');
    return std::to_string(sats / 100'000'000) + "." + frac;
}

std::optional<uint64_t> parse_coins(const std::string& text)
{
    if (text.empty() || text.size() > 30) return std::nullopt;
    const size_t dot{text.find('.')};
    const std::string whole{text.substr(0, dot)};
    const std::string frac{dot == std::string::npos ? std::string{} : text.substr(dot + 1)};
    if (whole.empty() && frac.empty()) return std::nullopt;
    if (frac.size() > 8) return std::nullopt;
    for (char c : whole + frac) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    if (dot != std::string::npos && frac.empty()) return std::nullopt;
    uint64_t w{0};
    for (char c : whole) {
        if (w > (UINT64_MAX - 9) / 10) return std::nullopt;
        w = w * 10 + static_cast<uint64_t>(c - '0');
    }
    if (w > UINT64_MAX / 100'000'000) return std::nullopt;
    uint64_t f{0};
    std::string padded{frac};
    padded.append(8 - padded.size(), '0');
    for (char c : padded) f = f * 10 + static_cast<uint64_t>(c - '0');
    const uint64_t total{w * 100'000'000};
    if (total > UINT64_MAX - f) return std::nullopt;
    return total + f;
}

WipeOnExit::~WipeOnExit() { sodium_memzero(text.data(), text.size()); }

uint64_t spend_total(std::initializer_list<uint64_t> parts)
{
    uint64_t total{0};
    for (const uint64_t part : parts) {
        if (part > MAX_MONEY) throw HostError(HOST_INVALID_REQUEST, "amount overflow: more than 21,000,000 ECX");
        uint64_t next{0};
        if (__builtin_add_overflow(total, part, &next) || next > MAX_MONEY) throw HostError(HOST_INVALID_REQUEST, "amount overflow: more than 21,000,000 ECX");
        total = next;
    }
    return total;
}

UniValue coins_value(uint64_t sats)
{
    UniValue v;
    v.setNumStr(format_sats(sats));
    return v;
}

// ------------------------------------------------------------------ LinkMonitor
LinkMonitor::LinkMonitor(enforcer::Client& client, uint8_t slot, std::function<std::optional<std::pair<Hash32, std::optional<uint32_t>>>()> followed)
    : client_(client), slot_(slot), followed_(std::move(followed))
{
}

LinkMonitor::~LinkMonitor() { stop(); }

void LinkMonitor::start()
{
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        while (running_) {
            look();
            for (int i{0}; i < 30 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void LinkMonitor::stop()
{
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

static std::string kind_of(const std::string& message)
{
    std::string m{message};
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (m.find("refused") != std::string::npos) return "refused";
    if (m.find("resolve") != std::string::npos || m.find("unreachable") != std::string::npos || m.find("connect") != std::string::npos) return "unreachable";
    if (m.find("timeout") != std::string::npos || m.find("timed out") != std::string::npos || m.find("deadline") != std::string::npos) return "timeout";
    if (m.find("unauth") != std::string::npos || m.find("permission") != std::string::npos) return "auth";
    return "other";
}

void LinkMonitor::look()
{
    auto r{client_.get_chain_tip()};
    const int64_t now{unix_now()};
    if (enforcer::ok(r)) {
        std::lock_guard<std::mutex> l(mu_);
        times_[enforcer::val(r).block_hash] = {enforcer::val(r).timestamp, enforcer::val(r).height};
    }
    // What the chain follows, asked before the lock (it may ask the enforcer for a block's height, through this monitor).
    std::optional<std::pair<Hash32, std::optional<uint32_t>>> followed;
    if (enforcer::ok(r) && followed_) followed = followed_();
    std::lock_guard<std::mutex> l(mu_);
    const std::string before{state_};
    if (!enforcer::ok(r)) {
        connected_ = false;
        error_ = enforcer::err(r).what();
        error_kind_ = kind_of(error_);
        last_error_ = now;
        state_ = "down";
    } else {
        connected_ = true;
        error_.clear();
        error_kind_.clear();
        last_ok_ = now;
        tip_ = enforcer::val(r);
        times_[tip_->block_hash] = {tip_->timestamp, tip_->height};
        // Syncing: the chain's tip acts on a mainchain block more than two below the mainchain's tip.
        state_ = "ok";
        if (followed && followed->second && tip_->height > *followed->second + 2) {
            state_ = "syncing";
            error_kind_ = "not-synced";
        }
    }
    if (state_ != before) since_ = now;
    if (times_.size() > 4096) times_.clear();
}

std::optional<uint64_t> LinkMonitor::tip_time() const
{
    std::lock_guard<std::mutex> l(mu_);
    if (!tip_) return std::nullopt;
    return tip_->timestamp;
}

bool LinkMonitor::connected() const
{
    std::lock_guard<std::mutex> l(mu_);
    return connected_;
}

UniValue LinkMonitor::info() const
{
    std::optional<std::pair<Hash32, std::optional<uint32_t>>> followed;
    if (followed_) followed = followed_();
    std::lock_guard<std::mutex> l(mu_);
    UniValue o{UniValue::VOBJ};
    o.pushKV("slot", slot_);
    o.pushKV("connected", connected_);
    if (!error_.empty()) o.pushKV("error", error_);
    o.pushKV("node", client_.url());
    o.pushKV("via", "enforcer");
    o.pushKV("height", tip_ ? static_cast<int64_t>(tip_->height) : int64_t{-1});
    if (tip_) o.pushKV("bestblockhash", hex_encode_rev(tip_->block_hash.span()));
    o.pushKV("tipheight", followed && followed->second ? static_cast<int64_t>(*followed->second) : int64_t{-1});
    o.pushKV("state", state_);
    if (!error_kind_.empty()) o.pushKV("error_kind", error_kind_);
    if (since_) o.pushKV("since", since_);
    if (last_ok_) o.pushKV("last_ok", last_ok_);
    if (last_error_) o.pushKV("last_error", last_error_);
    if (tip_) o.pushKV("mainchain_height", static_cast<int64_t>(tip_->height));
    if (followed && followed->second) o.pushKV("followed_height", static_cast<int64_t>(*followed->second));
    if (tip_ && followed && followed->second) o.pushKV("behind_blocks", static_cast<int64_t>(tip_->height) - static_cast<int64_t>(*followed->second));
    return o;
}

std::optional<uint64_t> LinkMonitor::block_time(const Hash32& main_block)
{
    {
        std::lock_guard<std::mutex> l(mu_);
        if (auto it = times_.find(main_block); it != times_.end()) return it->second.first;
    }
    auto r{client_.get_block_header_infos(main_block, 0)};
    if (!enforcer::ok(r) || enforcer::val(r).empty()) return std::nullopt;
    const auto& h{enforcer::val(r).front()};
    std::lock_guard<std::mutex> l(mu_);
    times_[main_block] = {h.timestamp, h.height};
    return h.timestamp;
}

std::optional<uint32_t> LinkMonitor::block_height(const Hash32& main_block)
{
    {
        std::lock_guard<std::mutex> l(mu_);
        if (auto it = times_.find(main_block); it != times_.end()) return it->second.second;
    }
    if (!block_time(main_block)) return std::nullopt;
    std::lock_guard<std::mutex> l(mu_);
    if (auto it = times_.find(main_block); it != times_.end()) return it->second.second;
    return std::nullopt;
}

// ------------------------------------------------------------------ AppStore
static ByteSpan text_span(const std::string& s) { return ByteSpan{reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

AppStore::AppStore(const std::string& path)
{
    fsperm::private_file(path, false);
    store_ = kv::open_sqlite(path);
}

std::optional<std::string> AppStore::get(const std::string& key) const
{
    kv::Txn t(*store_);
    auto v{t.get(1, text_span(key))};
    if (!v) return std::nullopt;
    return std::string(v->begin(), v->end());
}

void AppStore::put(const std::string& key, const std::string& value)
{
    kv::Txn t(*store_);
    t.put(1, text_span(key), text_span(value));
    t.commit();
}

void AppStore::erase(const std::string& key)
{
    kv::Txn t(*store_);
    t.del(1, text_span(key));
    t.commit();
}

// ------------------------------------------------------------------ PendingPhrases
void PendingPhrases::wipe(Entry& e) { sodium_memzero(e.phrase.data(), e.phrase.size()); }

std::string PendingPhrases::put(std::string phrase)
{
    const std::string id{random_hex(16)};
    const auto now{std::chrono::steady_clock::now()};
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires <= now) {
            wipe(it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    while (entries_.size() >= 8) {
        wipe(entries_.begin()->second);
        entries_.erase(entries_.begin());
    }
    entries_[id] = Entry{std::move(phrase), now + std::chrono::minutes{5}};
    return id;
}

std::optional<std::string> PendingPhrases::get(const std::string& id)
{
    std::lock_guard<std::mutex> l(mu_);
    auto it{entries_.find(id)};
    if (it == entries_.end() || it->second.expires <= std::chrono::steady_clock::now()) return std::nullopt;
    return it->second.phrase;
}

void PendingPhrases::erase(const std::string& id)
{
    std::lock_guard<std::mutex> l(mu_);
    if (auto it = entries_.find(id); it != entries_.end()) {
        wipe(it->second);
        entries_.erase(it);
    }
}

PendingPhrases::~PendingPhrases()
{
    for (auto& [id, e] : entries_) wipe(e);
}

// ------------------------------------------------------------------ Poller
SyncState sync_state(const NodeLook& look)
{
    SyncState s;
    s.height = look.height.value_or(0);
    if (look.peer_height) s.target = std::max(*look.peer_height, s.height);
    if (look.connections == 0 && !look.solo) {
        s.kind = SyncState::Waiting;
        return s;
    }
    // Block 0 is never up to date (a fresh data folder, or a node that has not had a block yet), nor a node still
    // reading the mainchain, nor one below what a peer has, nor one whose peers have not said yet (solo: its own).
    const bool behind{look.peer_height ? *look.peer_height > s.height : !look.solo};
    s.kind = s.height == 0 || look.mainchain_busy || behind ? SyncState::Syncing : SyncState::Synced;
    return s;
}

const char* sync_name(SyncState::Kind kind)
{
    switch (kind) {
    case SyncState::Synced: return "synced";
    case SyncState::Syncing: return "syncing";
    case SyncState::Waiting: return "waiting";
    }
    return "waiting";
}

UniValue tip_json(const NodeLook& look)
{
    const SyncState s{sync_state(look)};
    const bool synced{s.kind == SyncState::Synced};
    UniValue t{UniValue::VOBJ};
    t.pushKV("height", static_cast<int64_t>(s.height));
    // The time the kit's rule of a recent tip reads (lib/sync.ts): now when the node is up to date (a drop-in chain makes
    // blocks only when someone merge-mines one: its last block may be hours old on a chain fully synced), else its tip's.
    const auto now{static_cast<int64_t>(std::time(nullptr))};
    t.pushKV("time", synced ? std::max(now, static_cast<int64_t>(look.tip_time)) : static_cast<int64_t>(look.tip_time));
    t.pushKV("hash", look.tip);
    double progress{synced ? 1.0 : 0.0};
    if (!synced && s.target && *s.target > 0) progress = std::min(1.0, double(s.height) / double(*s.target));
    t.pushKV("progress", progress);
    t.pushKV("state", synced ? "synced" : "download");
    t.pushKV("ibd", !synced);
    t.pushKV("loadingBlocks", false);
    // The drop-in's own judgement (sync_state), which the kit shows as it is: "synced", "syncing" (towards target) or "waiting".
    t.pushKV("sync", sync_name(s.kind));
    t.pushKV("target", s.target ? static_cast<int64_t>(*s.target) : int64_t{-1});
    return t;
}

UniValue wallet_summary(const std::string& name)
{
    UniValue w{UniValue::VOBJ};
    w.pushKV("name", name);
    w.pushKV("encrypted", false);
    w.pushKV("locked", false);
    w.pushKV("privateKeysDisabled", false);
    w.pushKV("hd", true);
    return w;
}

Poller::Poller(EventHub& hub, std::string wallet_name, std::function<NodeLook()> look, std::chrono::milliseconds every)
    : hub_(hub), wallet_name_(std::move(wallet_name)), look_(std::move(look)), every_(every)
{
}

Poller::~Poller() { stop(); }

void Poller::start()
{
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        while (running_) {
            try {
                step();
            } catch (const std::exception& e) {
                LOG_DEBUG("app layer: a look at the node failed: %s", e.what());
            }
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, every_, [this] { return !running_ || poked_; });
            poked_ = false;
        }
    });
}

void Poller::stop()
{
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Poller::poke()
{
    {
        std::lock_guard<std::mutex> l(mu_);
        poked_ = true;
    }
    cv_.notify_all();
}

static UniValue balance_json(const std::string& name, const NodeLook& l)
{
    UniValue b{UniValue::VOBJ};
    b.pushKV("name", name);
    b.pushKV("trusted", coins_value(l.trusted));
    b.pushKV("untrusted_pending", coins_value(l.pending));
    b.pushKV("immature", coins_value(l.immature));
    return b;
}

void Poller::step()
{
    NodeLook now{look_()};
    std::optional<NodeLook> before;
    {
        std::lock_guard<std::mutex> l(mu_);
        before = last_;
        last_ = now;
    }
    const auto sync_key{[](const NodeLook& l) {
        const SyncState s{sync_state(l)};
        return std::make_tuple(l.tip, l.height, s.kind, s.target);
    }};
    if (!before || sync_key(*before) != sync_key(now)) hub_.emit("tip", tip_json(now), "tip");
    if (!before || before->connections != now.connections) {
        UniValue c{UniValue::VOBJ};
        c.pushKV("count", static_cast<uint64_t>(now.connections));
        hub_.emit("connections", c, "connections");
    }
    if (!before || before->warnings != now.warnings) {
        UniValue w{UniValue::VARR};
        for (const auto& s : now.warnings) w.push_back(s);
        UniValue a{UniValue::VOBJ};
        a.pushKV("warnings", w);
        hub_.emit("alert", a, "alert");
    }
    if (before && before->wallet != now.wallet) {
        if (now.wallet) {
            hub_.emit("wallet.loaded", wallet_summary(wallet_name_));
        } else {
            UniValue u{UniValue::VOBJ};
            u.pushKV("name", wallet_name_);
            hub_.emit("wallet.unloaded", u);
        }
    }
    if (now.wallet && (!before || !before->wallet || before->trusted != now.trusted || before->pending != now.pending || before->immature != now.immature)) {
        hub_.emit("wallet.balance", balance_json(wallet_name_, now), "wallet.balance:" + wallet_name_);
    }
    if (before && now.wallet) {
        for (const auto& [txid, confirmed] : now.txs) {
            auto it{before->txs.find(txid)};
            if (it != before->txs.end() && it->second == confirmed) continue;
            UniValue t{UniValue::VOBJ};
            t.pushKV("name", wallet_name_);
            t.pushKV("txid", txid);
            t.pushKV("change", it == before->txs.end() ? "new" : "confirmed");
            hub_.emit("wallet.tx", t);
        }
        for (const auto& [txid, confirmed] : before->txs) {
            if (now.txs.contains(txid)) continue;
            UniValue t{UniValue::VOBJ};
            t.pushKV("name", wallet_name_);
            t.pushKV("txid", txid);
            t.pushKV("change", "gone");
            hub_.emit("wallet.tx", t);
        }
    }
}

UniValue Poller::snapshot() const
{
    std::optional<NodeLook> l;
    {
        std::lock_guard<std::mutex> lock(mu_);
        l = last_;
    }
    NodeLook look{l.value_or(NodeLook{})};
    UniValue s{UniValue::VOBJ};
    s.pushKV("connections", static_cast<uint64_t>(look.connections));
    s.pushKV("networkActive", true);
    UniValue w{UniValue::VARR};
    for (const auto& x : look.warnings) w.push_back(x);
    s.pushKV("warnings", w);
    s.pushKV("tip", tip_json(look));
    // The headers: the best height known (the peers' best, sync_state's target), else the blocks' (a drop-in node takes a
    // block with its header).
    const UniValue tip{tip_json(look)};
    const int64_t target{tip.find_value("target").getInt<int64_t>()};
    UniValue h{UniValue::VOBJ};
    h.pushKV("height", std::max(target, tip.find_value("height").getInt<int64_t>()));
    h.pushKV("time", tip.find_value("time"));
    s.pushKV("headerTip", h);
    UniValue wallets{UniValue::VARR};
    if (look.wallet) wallets.push_back(wallet_summary(wallet_name_));
    s.pushKV("wallets", wallets);
    UniValue balances{UniValue::VOBJ};
    if (look.wallet) balances.pushKV(wallet_name_, balance_json(wallet_name_, look));
    s.pushKV("balances", balances);
    return s;
}

} // namespace bt::appui
