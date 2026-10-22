// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/appui/host.h>

#include <bt/bytes.h>
#include <bt/crypto.h>
#include <bt/log.h>

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bt::appui {

namespace {
const UniValue NULLV;

std::string one_line(std::string s, size_t max)
{
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (s.size() > max) s = s.substr(0, max) + "…";
    return s;
}

const UniValue& field(const UniValue& o, const char* key) { return o.isObject() ? o.find_value(key) : NULLV; }
} // namespace

UniValue question_json(const Question& q)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("title", q.title);
    out.pushKV("message", q.message);
    out.pushKV("accept", q.accept);
    out.pushKV("cancel", q.cancel);
    out.pushKV("details", q.details);
    out.pushKV("level", q.level);
    out.pushKV("kind", q.kind);
    UniValue rows{UniValue::VARR};
    for (const Row& r : q.rows) {
        UniValue o{UniValue::VOBJ};
        o.pushKV("label", r.label);
        o.pushKV("value", r.value);
        o.pushKV("kind", r.kind);
        rows.push_back(std::move(o));
    }
    out.pushKV("rows", std::move(rows));
    out.pushKV("passphrase", q.passphrase);
    out.pushKV("passphraseLabel", q.passphrase_label);
    out.pushKV("error", q.error);
    return out;
}

std::string digest(const std::string& request, const Question& q)
{
    const std::string question{question_json(q).write()};
    // Each part with its length first: no two pairs of texts make the same bytes.
    const std::string bytes{std::to_string(request.size()) + ":" + request + std::to_string(question.size()) + ":" + question};
    return hex_encode(sha256(ByteSpan{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}));
}

bool has_duplicate_keys(const UniValue& value)
{
    if (value.isObject()) {
        std::set<std::string> seen;
        for (size_t i{0}; i < value.size(); ++i) {
            if (!seen.insert(value.getKeys()[i]).second) return true;
            if (has_duplicate_keys(value.getValues()[i])) return true;
        }
    } else if (value.isArray()) {
        for (const UniValue& v : value.getValues()) {
            if (has_duplicate_keys(v)) return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ Bindings
std::string Bindings::text(Check c)
{
    switch (c) {
    case Check::OK: return "ok";
    case Check::UNKNOWN: return "The confirmation is not known (used already, or never given): nothing was run.";
    case Check::EXPIRED: return "The confirmation came too late: nothing was run. Ask again.";
    case Check::REQUEST_CHANGED: return "The call is not the one that was confirmed: nothing was run.";
    case Check::QUESTION_CHANGED: return "What the call would do changed since it was confirmed: nothing was run. Ask again.";
    case Check::WALLET_CHANGED: return "The wallet is not the one the call was confirmed for: nothing was run.";
    }
    return "?";
}

namespace {
//! What a token keeps of its request: its SHA-256 (enough to tell it is the same request; a restore's typed phrase is
//! in the request, which is not kept).
std::string request_hash(const std::string& request)
{
    unsigned char h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(request.data()), request.size());
    static constexpr char HEX[]{"0123456789abcdef"};
    std::string out;
    for (const unsigned char c : h) {
        out += HEX[c >> 4];
        out += HEX[c & 15];
    }
    return out;
}
} // namespace

std::string Bindings::issue(const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now)
{
    const std::string token{random_hex(32)};
    std::lock_guard<std::mutex> l(mu_);
    std::erase_if(entries_, [&](const auto& e) { return e.second.expires <= now; });
    while (entries_.size() >= MAX_TOKENS) {
        const auto oldest{std::ranges::min_element(entries_, {}, [](const auto& e) { return e.second.order; })};
        entries_.erase(oldest);
    }
    entries_.emplace(token, Entry{request_hash(request), digest, wallet_id, now + TOKEN_EXPIRY, next_++});
    return token;
}

Bindings::Check Bindings::take(const std::string& token, const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> l(mu_);
    const auto it{entries_.find(token)};
    if (it == entries_.end()) return Check::UNKNOWN;
    const Entry entry{it->second};
    entries_.erase(it);
    if (entry.expires <= now) return Check::EXPIRED;
    if (entry.request != request_hash(request)) return Check::REQUEST_CHANGED;
    if (entry.wallet_id != wallet_id) return Check::WALLET_CHANGED;
    if (entry.digest != digest) return Check::QUESTION_CHANGED;
    return Check::OK;
}

size_t Bindings::size() const
{
    std::lock_guard<std::mutex> l(mu_);
    return entries_.size();
}

// ------------------------------------------------------------------ EventLog
void EventLog::add(std::string batch)
{
    {
        std::lock_guard<std::mutex> l(mu_);
        batches_.emplace_back(++seq_, std::move(batch));
        while (batches_.size() > MAX_BATCHES) batches_.pop_front();
    }
    cv_.notify_all();
}

UniValue EventLog::after(uint64_t since, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, timeout, [&] { return stopped_ || seq_ > since; });
    UniValue out{UniValue::VOBJ};
    UniValue list{UniValue::VARR};
    // Asked for batches older than are kept (or a number of another run): all those kept, said.
    const bool reset{since > seq_ || (!batches_.empty() && batches_.front().first > since + 1)};
    for (const auto& [n, b] : batches_) {
        if (reset || n > since) list.push_back(b);
    }
    out.pushKV("seq", seq_);
    out.pushKV("reset", reset);
    out.pushKV("batches", std::move(list));
    return out;
}

void EventLog::stop()
{
    {
        std::lock_guard<std::mutex> l(mu_);
        stopped_ = true;
    }
    cv_.notify_all();
}

// ------------------------------------------------------------------ EventHub
EventHub::EventHub(EventLog& log) : log_(log)
{
    thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mu_);
        while (!stopped_) {
            cv_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
            if (stopped_) break;
            // A moment to gather what comes together (a block: the tip, a balance, a transaction).
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopped_; });
            lock.unlock();
            flush();
            lock.lock();
        }
    });
}

EventHub::~EventHub() { stop(); }

void EventHub::emit(const std::string& type, UniValue data, const std::string& key)
{
    {
        std::lock_guard<std::mutex> l(mu_);
        if (stopped_) return;
        if (!key.empty()) {
            if (auto it = keyed_.find(key); it != keyed_.end()) {
                pending_[it->second] = {type, std::move(data)};
                return;
            }
            keyed_[key] = pending_.size();
        }
        pending_.emplace_back(type, std::move(data));
    }
    cv_.notify_all();
}

void EventHub::flush()
{
    std::vector<std::pair<std::string, UniValue>> events;
    {
        std::lock_guard<std::mutex> l(mu_);
        events.swap(pending_);
        keyed_.clear();
    }
    if (events.empty()) return;
    UniValue batch{UniValue::VARR};
    for (auto& [type, data] : events) {
        UniValue e{UniValue::VOBJ};
        e.pushKV("type", type);
        e.pushKV("data", std::move(data));
        batch.push_back(std::move(e));
    }
    log_.add(batch.write());
}

void EventHub::stop()
{
    {
        std::lock_guard<std::mutex> l(mu_);
        if (stopped_) return;
        stopped_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

// ------------------------------------------------------------------ ChosenFiles
struct ChosenFiles::Held {
#ifdef _WIN32
    HANDLE file{INVALID_HANDLE_VALUE};
    ~Held()
    {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
#else
    //! The file (to read, or to write over), or the folder a new file is made in.
    int fd{-1};
    bool folder{false};
    std::string name;
    ~Held()
    {
        if (fd >= 0) ::close(fd);
    }
#endif
};

std::string ChosenFiles::problem(const std::string& path, const std::string& folder)
{
    if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos) return "not a path";
    const std::filesystem::path p{path};
    if (!p.is_absolute()) return "not an absolute path";
    std::error_code ec;
    const std::filesystem::path canon{std::filesystem::weakly_canonical(p, ec)};
    if (ec) return "the path cannot be resolved";
    if (canon.string() != path) return "not a canonical path (a link, \"..\" or \".\" in it)";
    if (std::filesystem::is_symlink(p, ec)) return "a symbolic link";
    if (std::filesystem::exists(p, ec) && !std::filesystem::is_regular_file(p, ec)) return "not a file";
    if (!folder.empty()) {
        const std::filesystem::path base{std::filesystem::weakly_canonical(std::filesystem::path{folder}, ec)};
        if (ec || p.parent_path() != base) return "not a file of the folder for the files of the app";
    }
    return {};
}

std::string ChosenFiles::grant(const std::string& path, bool write, bool overwrite, const std::string& user, std::chrono::steady_clock::time_point now,
                               const std::string& folder)
{
    if (const std::string why{problem(path, folder)}; !why.empty()) throw std::runtime_error(why);
    auto held{std::make_shared<Held>()};
#ifdef _WIN32
    const std::wstring w{std::filesystem::path{path}.wstring()};
    const bool exists{GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES};
    if (write && exists && !overwrite) throw std::runtime_error("there is a file there already");
    if (!write && !exists) throw std::runtime_error("there is no such file");
    if (exists) {
        held->file = CreateFileW(w.c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (held->file == INVALID_HANDLE_VALUE) throw std::runtime_error("it cannot be opened");
    }
#else
    struct stat st{};
    const bool exists{::lstat(path.c_str(), &st) == 0};
    if (exists && !S_ISREG(st.st_mode)) throw std::runtime_error("not a file");
    if (write && exists && !overwrite) throw std::runtime_error("there is a file there already");
    if (!write && !exists) throw std::runtime_error("there is no such file");
    if (exists) {
        held->fd = ::open(path.c_str(), (write ? O_WRONLY : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
        if (held->fd < 0) throw std::runtime_error("it cannot be opened");
    } else {
        const std::filesystem::path p{path};
        held->fd = ::open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (held->fd < 0) throw std::runtime_error("its folder cannot be opened");
        held->folder = true;
        held->name = p.filename().string();
    }
#endif
    const std::string token{random_hex(32)};
    std::lock_guard<std::mutex> l(mu_);
    std::erase_if(entries_, [&](const auto& e) { return e.second.expires <= now; });
    while (entries_.size() >= MAX) entries_.erase(std::ranges::min_element(entries_, {}, [](const auto& e) { return e.second.expires; }));
    entries_.emplace(token, Entry{File{path, write, overwrite, user, held}, now + EXPIRY});
    return token;
}

std::optional<ChosenFiles::File> ChosenFiles::take(const std::string& token, const std::string& user, std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> l(mu_);
    const auto it{entries_.find(token)};
    if (it == entries_.end()) return std::nullopt;
    Entry e{it->second};
    if (e.file.user != user) return std::nullopt;
    entries_.erase(it);
    if (e.expires <= now) return std::nullopt;
    return e.file;
}

std::string ChosenFiles::read(const File& f, size_t max)
{
    if (!f.held) throw std::runtime_error("not opened");
    std::string out;
#ifdef _WIN32
    if (f.held->file == INVALID_HANDLE_VALUE) throw std::runtime_error("not opened");
    char buf[65536];
    DWORD n{0};
    while (ReadFile(f.held->file, buf, sizeof(buf), &n, nullptr) && n > 0) {
        out.append(buf, n);
        if (out.size() > max) throw std::runtime_error("the file is too large");
    }
#else
    if (f.held->folder || f.held->fd < 0) throw std::runtime_error("not a file to read");
    if (::lseek(f.held->fd, 0, SEEK_SET) < 0) throw std::runtime_error("cannot read it");
    char buf[65536];
    for (;;) {
        const ssize_t n{::read(f.held->fd, buf, sizeof(buf))};
        if (n < 0) throw std::runtime_error("cannot read it");
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > max) throw std::runtime_error("the file is too large");
    }
#endif
    return out;
}

void ChosenFiles::write(const File& f, const std::string& data)
{
    if (!f.held || !f.write) throw std::runtime_error("not chosen to be written");
#ifdef _WIN32
    HANDLE h{f.held->file};
    bool made{false};
    if (h == INVALID_HANDLE_VALUE) {
        // Owner-only, as on POSIX (0600): a backup phrase saved into a shared folder is not readable by the others.
        PSECURITY_DESCRIPTOR sd{nullptr};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) {
            throw std::runtime_error("cannot make its access list");
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        h = CreateFileW(std::filesystem::path{std::u8string(f.path.begin(), f.path.end())}.wstring().c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        LocalFree(sd);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("it cannot be made (a file appeared there?)");
        made = true;
    } else {
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
    }
    DWORD n{0};
    const bool ok{WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) && n == data.size() && FlushFileBuffers(h)};
    if (made) CloseHandle(h);
    if (!ok) throw std::runtime_error("cannot write it");
#else
    int fd{f.held->fd};
    bool made{false};
    if (f.held->folder) {
        fd = ::openat(f.held->fd, f.held->name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) throw std::runtime_error("it cannot be made (a file appeared there?)");
        made = true;
    } else if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
        throw std::runtime_error("cannot write it");
    }
    size_t off{0};
    bool ok{true};
    while (off < data.size()) {
        const ssize_t n{::write(fd, data.data() + off, data.size() - off)};
        if (n <= 0) {
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (made) ::close(fd);
    if (!ok) throw std::runtime_error("cannot write it");
#endif
}

// ------------------------------------------------------------------ requests
Request parse_request(const UniValue& request)
{
    if (!request.isObject()) throw RpcError(RPC_INVALID_PARAMETER, "request: {kind, method, params, wallet?}");
    if (has_duplicate_keys(request)) throw RpcError(RPC_INVALID_PARAMETER, "request: a key is given twice");
    for (const std::string& key : request.getKeys()) {
        if (key != "kind" && key != "method" && key != "params" && key != "wallet") throw RpcError(RPC_INVALID_PARAMETER, "request: no field " + key);
    }
    const UniValue& kind{request.find_value("kind")};
    const UniValue& method{request.find_value("method")};
    const UniValue& params{request.find_value("params")};
    const UniValue& wallet{request.find_value("wallet")};
    if (!kind.isStr() || (kind.get_str() != "rpc" && kind.get_str() != "host")) throw RpcError(RPC_INVALID_PARAMETER, "request.kind: rpc or host");
    if (!method.isStr() || method.get_str().empty()) throw RpcError(RPC_INVALID_PARAMETER, "request.method: a name");
    if (!params.isNull() && !params.isArray() && !params.isObject()) throw RpcError(RPC_INVALID_PARAMETER, "request.params: an array or an object");
    if (!wallet.isNull() && !wallet.isStr()) throw RpcError(RPC_INVALID_PARAMETER, "request.wallet: a name");
    Request out{kind.get_str(), method.get_str(), params.isNull() ? UniValue{kind.get_str() == "rpc" ? UniValue::VARR : UniValue::VOBJ} : params,
                wallet.isStr() ? wallet.get_str() : std::string{}};
    if (out.kind == "host") {
        // One wallet per request: the one of its parameters; a wallet beside them is that one, or the request is refused.
        const UniValue& inner{field(out.params, "wallet")};
        if (!inner.isNull() && !inner.isStr()) throw RpcError(RPC_INVALID_PARAMETER, "request.params.wallet: a name");
        const std::string acts_on{inner.isStr() ? inner.get_str() : std::string{}};
        if (wallet.isStr() && (inner.isNull() || out.wallet != acts_on)) {
            throw RpcError(RPC_INVALID_PARAMETER, "request.wallet: a host method acts on the wallet of its parameters (params.wallet); a request naming another is refused");
        }
        out.wallet = acts_on;
    }
    return out;
}

// ------------------------------------------------------------------ RemoteHost
struct RemoteHost::Context {
    std::string request;
    std::string wallet;
    std::string token;
    std::string passphrase;
    std::string user;
    std::map<std::string, ChosenFiles::File> files;
    bool describe_only{false};
    bool accepted{false};
    ~Context()
    {
        // The request may carry a phrase typed for a restore: not left in freed memory.
        sodium_memzero(passphrase.data(), passphrase.size());
        sodium_memzero(request.data(), request.size());
    }
};

std::string RemoteHost::canonical(const std::string& kind, const std::string& method, const UniValue& params, const std::string& wallet)
{
    UniValue c{UniValue::VARR};
    c.push_back(kind);
    c.push_back(method);
    c.push_back(params);
    c.push_back(wallet);
    return c.write();
}

RemoteHost::RemoteHost(ChainHost chain, HostOptions options, RpcServer& server)
    : chain_(std::move(chain)), options_(std::move(options)), server_(server), hub_(log_)
{
    // What the host answers itself: app.hello and app.info say what the chain and the node are (no wallet, no amount),
    // rpc.confirmedMethods names the methods that ask, rpc.confirmed checks the method it runs by name; node.state
    // shows the chain's state (the read that stands for it), its wallet amounts only to a caller of the wallet read.
    for (const char* m : {"app.hello", "app.info", "rpc.confirmed", "rpc.confirmedMethods"}) chain_.privileges.try_emplace(m);
    chain_.privileges.try_emplace("node.state", std::vector<std::string>{chain_.chain_read});
    // Who may read which event: the chain's to a reader of the chain, the wallet's to a reader of the wallet.
    for (const char* e : {"tip", "headerTip", "connections", "networkActive", "alert", "progress", "init.message", "bans", "mempool", "traffic"}) {
        chain_.event_privileges.try_emplace(e, std::vector<std::string>{chain_.chain_read});
    }
    for (const char* e : {"wallet.loaded", "wallet.unloaded", "wallet.status", "wallet.balance", "wallet.progress", "wallet.addressbook"}) {
        chain_.event_privileges.try_emplace(e, std::vector<std::string>{chain_.wallet_read});
    }
    chain_.event_privileges.try_emplace("wallet.tx", std::vector<std::string>{chain_.wallet_read, chain_.owner_read});
    server_.add("webuihost", {"request", "token", "passphrase", "files"}, [this](const Call& c) {
        std::string passphrase{c[2].isStr() ? c[2].get_str() : std::string{}};
        UniValue out{run(c[0], c[1].isStr() ? c[1].get_str() : std::string{}, passphrase, c[3], c.user)};
        std::fill(passphrase.begin(), passphrase.end(), '\0');
        return out;
    });
    server_.add("describeconfirmation", {"method", "params", "wallet"}, [this](const Call& c) {
        if (!c[0].isStr()) throw RpcError(RPC_INVALID_PARAMETER, "method: a name");
        const UniValue params{c[1].isNull() ? UniValue{UniValue::VARR} : c[1]};
        if (has_duplicate_keys(params)) throw RpcError(RPC_INVALID_PARAMETER, "params: a key is given twice");
        return describe(c[0].get_str(), params, c.user);
    });
    server_.add("webuievents", {"since", "timeout"}, [this](const Call& c) {
        const int64_t since{c[0].isNum() ? c[0].getInt<int64_t>() : 0};
        const int64_t timeout{c[1].isNum() ? std::clamp<int64_t>(c[1].getInt<int64_t>(), 0, 30000) : 0};
        if (stopped_) throw RpcError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        return events_for(static_cast<uint64_t>(std::max<int64_t>(0, since)), std::chrono::milliseconds{timeout}, c.user);
    });
    // Who answers: the proof that this server holds the cookie of the node's data folder (auth.h server_proof).
    server_.add("appserverproof", {"challenge"}, [this](const Call& c) {
        if (!c[0].isStr() || c[0].get_str().size() < 32 || c[0].get_str().size() > 128) throw RpcError(RPC_INVALID_PARAMETER, "challenge: 32 to 128 characters");
        if (server_.cookie().empty()) throw RpcError(RPC_MISC_ERROR, "No cookie");
        return UniValue{server_proof(server_.cookie(), c[0].get_str())};
    });
    server_.add("webuichosenfile", {"path", "write", "overwrite"}, [this](const Call& c) {
        if (!c[0].isStr()) throw RpcError(RPC_INVALID_PARAMETER, "path: the file chosen");
        // Only the app's own chooser: its RPC user, or one that may call everything.
        if ((options_.chooser_user.empty() || c.user != options_.chooser_user) && !server_.may_call_all(c.user)) {
            throw RpcError(HOST_NOT_ALLOWED, "Only the app's own chooser gives files (--bt-app-chooser): this RPC user may not");
        }
        try {
            return UniValue{files_.grant(c[0].get_str(), c[1].isTrue(), c[2].isTrue(), c.user, std::chrono::steady_clock::now(), options_.file_dir)};
        } catch (const std::runtime_error& e) {
            throw RpcError(RPC_INVALID_PARAMETER, std::string{"This file cannot be used: "} + e.what());
        }
    });
}

RemoteHost::~RemoteHost() { stop(); }

void RemoteHost::stop()
{
    if (stopped_.exchange(true)) return;
    log_.stop();
    hub_.stop();
}

UniValue RemoteHost::info() const
{
    UniValue i{chain_.info ? chain_.info() : UniValue{UniValue::VOBJ}};
    if (!i.isObject()) i = UniValue{UniValue::VOBJ};
    i.pushKV("embedded", true);
    i.pushKV("remote", true);
    i.pushKV("hostApi", HOST_API);
    return i;
}

bool RemoteHost::host_method_allowed(const std::string& method, const std::function<bool(const std::string&)>& allowed) const
{
    if (allowed(method)) return true;
    const auto it{chain_.privileges.find(method)};
    return it != chain_.privileges.end() && std::ranges::all_of(it->second, allowed);
}

Answer RemoteHost::ask(const Question& q, Context& ctx)
{
    // A wrong passphrase: said, not asked again in the same request (the app asks again).
    if (ctx.accepted && !q.error.empty()) throw HostError(-14, q.error);
    // A second question after a yes: never asked (answering it would run what the first allowed once more).
    if (ctx.accepted) throw HostError(HOST_INVALID_REQUEST, one_line(q.title, 80) + ": one confirmation per call: run them one at a time");
    if (q.rows.size() > MAX_ROWS) throw HostError(HOST_INVALID_REQUEST, "Too much for one question");
    const std::string d{digest(ctx.request, q)};
    const auto now{std::chrono::steady_clock::now()};
    const std::string wallet_id{chain_.wallet_id ? chain_.wallet_id() : std::string{}};
    if (!ctx.token.empty() && !ctx.describe_only) {
        const Bindings::Check check{bindings_.take(ctx.token, ctx.request, d, wallet_id, now)};
        if (check != Bindings::Check::OK) {
            LOG_INFO("app host: a confirmation was refused (%s): %s", Bindings::text(check).c_str(), one_line(q.title, 80).c_str());
            throw HostError(CONFIRMATION_REFUSED, Bindings::text(check));
        }
        ctx.accepted = true;
        LOG_INFO("app host: confirmed by the app: %s", one_line(q.title, 80).c_str());
        return Answer{true, ctx.passphrase};
    }
    UniValue description{question_json(q)};
    description.pushKV("token", bindings_.issue(ctx.request, d, wallet_id, now));
    description.pushKV("digest", d);
    description.pushKV("expiresIn", static_cast<int64_t>(TOKEN_EXPIRY.count()));
    description.pushKV("chain", chain_.name);
    description.pushKV("network", chain_.network);
    throw NeedConfirmation{std::move(description)};
}

UniValue RemoteHost::call_rpc(const std::string& method, const UniValue& params, const std::string& wallet, Context& ctx)
{
    // The pages' own policy: what they read. What pays or signs is rpc.confirmed, asked.
    if (!chain_.page_reads.contains(method)) throw HostError(HOST_METHOD_NOT_FOUND, "The interface does not call " + method);
    if (!server_.allowed(ctx.user, method)) throw HostError(HOST_NOT_ALLOWED, "This RPC user may not call " + method + " (-rpcwhitelist)");
    Call call;
    call.method = method;
    call.params = params.isArray() ? params : UniValue{UniValue::VARR};
    call.uri = wallet.empty() ? "/" : "/wallet/" + wallet;
    call.user = ctx.user;
    try {
        return server_.dispatch(call);
    } catch (const RpcError& e) {
        throw HostError(e.code, e.what());
    }
}

UniValue RemoteHost::call_host(const std::string& method, const UniValue& params, Context& ctx)
{
    const auto may{[this, &ctx](const std::string& m) { return server_.allowed(ctx.user, m); }};
    if (!host_method_allowed(method, may)) {
        std::string needs;
        if (auto it = chain_.privileges.find(method); it != chain_.privileges.end()) {
            for (const std::string& m : it->second) needs += (needs.empty() ? "" : ", ") + m;
        }
        throw HostError(HOST_NOT_ALLOWED, "This RPC user may not run " + method + " (-rpcwhitelist: neither it nor what it does, " +
                                              (needs.empty() ? std::string{"only by its own name"} : needs) + ", is listed)");
    }
    if (method == "app.hello") {
        UniValue out{UniValue::VOBJ};
        out.pushKV("phase", "ready");
        out.pushKV("phaseData", UniValue{UniValue::VOBJ});
        out.pushKV("dialogs", UniValue{UniValue::VARR});
        out.pushKV("info", info());
        return out;
    }
    if (method == "app.info") return info();
    if (method == "node.state") {
        if (!chain_.snapshot) throw HostError(HOST_METHOD_NOT_FOUND, "No node state");
        UniValue s{chain_.snapshot()};
        if (s.isObject() && !may(chain_.wallet_read)) {
            // The wallet's amounts, and whether there is a wallet at all, only to a reader of the wallet.
            UniValue kept{UniValue::VOBJ};
            for (size_t i{0}; i < s.size(); ++i) {
                if (s.getKeys()[i] != "balances" && s.getKeys()[i] != "wallets") kept.pushKV(s.getKeys()[i], s.getValues()[i]);
            }
            s = std::move(kept);
        }
        return s;
    }
    HostEnv env;
    env.user = ctx.user;
    env.ask = [this, &ctx](const Question& q) { return ask(q, ctx); };
    env.may_call = may;
    env.file = [&ctx](const std::string& path, bool write) {
        const auto it{ctx.files.find(path)};
        if (it == ctx.files.end() || (write && !it->second.write)) throw HostError(HOST_INVALID_REQUEST, "Only a file chosen by the user can be used");
        return it->second;
    };
    if (method == "rpc.confirmedMethods") {
        UniValue out{UniValue::VARR};
        for (const auto& [name, _] : chain_.confirmed) out.push_back(name);
        return out;
    }
    if (method == "rpc.confirmed") {
        const UniValue& inner{field(params, "method")};
        if (!inner.isStr()) throw HostError(HOST_INVALID_REQUEST, "method: the name of a confirmed method");
        const auto it{chain_.confirmed.find(inner.get_str())};
        if (it == chain_.confirmed.end()) throw HostError(HOST_INVALID_REQUEST, inner.get_str() + " is not a method that is confirmed");
        // What it runs by name, only what the caller may call itself.
        if (!may(inner.get_str())) throw HostError(HOST_NOT_ALLOWED, "This RPC user may not call " + inner.get_str() + " (-rpcwhitelist)");
        const UniValue& p{field(params, "params")};
        const UniValue args{p.isNull() ? UniValue{UniValue::VARR} : p};
        // The question and the run read the parameters in their one form; the token binds the request as sent.
        const UniValue canon{it->second.canonical ? it->second.canonical(args) : args};
        const Question q{it->second.describe(canon)};
        // Asked (or answered by a token) before anything runs.
        const Answer a{env.ask(q)};
        if (!a.accepted) throw HostError(HOST_CANCELLED, "Cancelled");
        if (ctx.describe_only) throw HostError(HOST_AUTH_NEEDED, "described only");
        return it->second.run(canon);
    }
    const auto it{chain_.host.find(method)};
    if (it == chain_.host.end()) throw HostError(HOST_METHOD_NOT_FOUND, "No host method " + method + " in the node");
    return it->second(params, env);
}

UniValue RemoteHost::run(const UniValue& request, const std::string& token, const std::string& passphrase, const UniValue& files, const std::string& user)
{
    if (stopped_) throw RpcError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
    Context ctx;
    const Request r{parse_request(request)};
    ctx.request = canonical(r.kind, r.method, r.params, r.wallet);
    ctx.wallet = r.wallet;
    ctx.token = token;
    ctx.passphrase = passphrase;
    ctx.user = user;
    if (files.isArray()) {
        for (const UniValue& f : files.getValues()) {
            const auto file{f.isStr() ? files_.take(f.get_str(), user, std::chrono::steady_clock::now()) : std::nullopt};
            if (!file) throw RpcError(RPC_INVALID_PARAMETER, "files: a token of a file chosen in the app is needed (unknown, used or expired)");
            if (!ctx.files.emplace(file->path, *file).second) throw RpcError(RPC_INVALID_PARAMETER, "files: one token for a file");
        }
    }
    UniValue out{UniValue::VOBJ};
    try {
        UniValue result{r.kind == "rpc" ? call_rpc(r.method, r.params, r.wallet, ctx) : call_host(r.method, r.params, ctx)};
        out.pushKV("result", std::move(result));
    } catch (NeedConfirmation& need) {
        out.pushKV("confirm", std::move(need.description));
    } catch (const HostError& e) {
        throw RpcError(e.code, e.what());
    } catch (const RpcError&) {
        throw;
    } catch (const std::exception& e) {
        throw RpcError(HOST_ERROR, e.what());
    }
    return out;
}

UniValue RemoteHost::events_for(uint64_t since, std::chrono::milliseconds timeout, const std::string& user)
{
    if (server_.may_call_all(user)) return log_.after(since, timeout);
    // A caller who may see none of what came waits on until the timeout (and gets the new seq then): the moment its
    // wait ends tells nothing of events it may not read (the wallet's activity).
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    for (;;) {
        const auto left{std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())};
        UniValue filtered{filter_events(log_.after(since, std::max(left, std::chrono::milliseconds{0})), user)};
        if (left.count() <= 0 || filtered.find_value("reset").isTrue() || !filtered.find_value("batches").empty() || stopped_) return filtered;
        const uint64_t seq{filtered.find_value("seq").getInt<uint64_t>()};
        if (seq == since) return filtered; // the wait ended with nothing new: its timeout
        since = seq;
    }
}

UniValue RemoteHost::filter_events(const UniValue& out, const std::string& user)
{
    // Each event only to a caller of the read it stands for (event_privileges); a type not listed to nobody.
    std::map<std::string, bool> may;
    auto allowed = [&](const std::string& type) {
        auto it{may.find(type)};
        if (it != may.end()) return it->second;
        const auto need{chain_.event_privileges.find(type)};
        const bool ok{need != chain_.event_privileges.end() &&
                      std::ranges::all_of(need->second, [&](const std::string& m) { return server_.allowed(user, m); })};
        may[type] = ok;
        return ok;
    };
    UniValue batches{UniValue::VARR};
    for (const UniValue& b : out.find_value("batches").getValues()) {
        UniValue events;
        if (!events.read(b.get_str()) || !events.isArray()) continue;
        UniValue kept{UniValue::VARR};
        for (const UniValue& e : events.getValues()) {
            const UniValue& type{e.isObject() ? e.find_value("type") : NULLV};
            if (type.isStr() && allowed(type.get_str())) kept.push_back(e);
        }
        if (!kept.empty()) batches.push_back(kept.write());
    }
    UniValue filtered{UniValue::VOBJ};
    filtered.pushKV("seq", out.find_value("seq"));
    filtered.pushKV("reset", out.find_value("reset"));
    filtered.pushKV("batches", batches);
    return filtered;
}

UniValue RemoteHost::describe(const std::string& method, const UniValue& params, const std::string& user)
{
    if (stopped_) throw RpcError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
    if (!server_.allowed(user, method)) throw RpcError(HOST_NOT_ALLOWED, "This RPC user may not call " + method + " (-rpcwhitelist)");
    const auto it{chain_.confirmed.find(method)};
    if (it == chain_.confirmed.end()) throw RpcError(RPC_INVALID_PARAMETER, method + " is not a method that is confirmed: it reads, or is not offered to the pages");
    UniValue canon;
    try {
        canon = it->second.canonical ? it->second.canonical(params) : params;
    } catch (const HostError& e) {
        throw RpcError(RPC_INVALID_PARAMETER, e.what());
    }
    UniValue inner{UniValue::VOBJ};
    inner.pushKV("method", method);
    inner.pushKV("params", canon);
    UniValue request{UniValue::VOBJ};
    request.pushKV("kind", "host");
    request.pushKV("method", "rpc.confirmed");
    request.pushKV("params", inner);
    Context ctx;
    ctx.request = canonical("host", "rpc.confirmed", inner, "");
    ctx.describe_only = true;
    ctx.user = user;
    UniValue out{UniValue::VOBJ};
    try {
        call_host("rpc.confirmed", inner, ctx);
        out.pushKV("needed", false);
    } catch (NeedConfirmation& need) {
        out = std::move(need.description);
        out.pushKV("needed", true);
    } catch (const HostError& e) {
        if (e.code != HOST_AUTH_NEEDED) throw RpcError(e.code, e.what());
        out.pushKV("needed", false);
    }
    out.pushKV("request", std::move(request));
    return out;
}

} // namespace bt::appui
