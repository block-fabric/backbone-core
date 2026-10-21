// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/jsonrpc.h>
#include <bt/parse.h>

#include <algorithm>

#include <cstring>
#include <sstream>
#include <variant>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void sclose(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void sclose(sock_t s) { ::close(s); }
#endif

namespace bt::jsonrpc {

static const UniValue NULLV;

const UniValue& Params::operator[](size_t i) const { return i < v.size() ? v[i] : NULLV; }

void Server::add(const std::string& method, std::vector<std::string> names, size_t required, Handler h)
{
    methods_[method] = Method{std::move(names), required, std::move(h)};
}

std::vector<std::string> Server::methods() const
{
    std::vector<std::string> r;
    for (auto& [k, _] : methods_) r.push_back(k);
    return r;
}

static UniValue error_obj(int code, const std::string& msg, const std::optional<std::string>& data = std::nullopt)
{
    UniValue e(UniValue::VOBJ);
    e.pushKV("code", code);
    e.pushKV("message", msg);
    if (data) e.pushKV("data", *data);
    return e;
}

// jsonrpsee writes "jsonrpc", "id", then "result" or "error" (their Response's field order), byte for byte.
static UniValue response(const UniValue& id, const UniValue* result, const UniValue* error)
{
    UniValue r(UniValue::VOBJ);
    r.pushKV("jsonrpc", "2.0");
    r.pushKV("id", id);
    if (result) r.pushKV("result", *result);
    if (error) r.pushKV("error", *error);
    return r;
}

namespace {
constexpr size_t MAX_BATCH = 1000;
// A scanner over the raw JSON text, to find where a value is (serde_json reports positions in the params text).
struct RawJson {
    std::string_view s;
    size_t i = 0;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool string(std::string* out = nullptr)
    {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                if (++i >= s.size()) return false;
                if (out && s[i] != 'u') out->push_back(s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i] == 'r' ? '\r' : s[i] == 'b' ? '\b' : s[i] == 'f' ? '\f' : s[i]);
                if (s[i] == 'u') { if (out) out->push_back('?'); i += 4; }
                i++;
                continue;
            }
            if (out) out->push_back(s[i]);
            i++;
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    /** Skips one value; false if malformed. */
    bool value()
    {
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '"') return string();
        if (c == '{' || c == '[') {
            char close = c == '{' ? '}' : ']';
            i++;
            ws();
            if (i < s.size() && s[i] == close) { i++; return true; }
            while (true) {
                if (c == '{') {
                    ws();
                    if (!string()) return false;
                    ws();
                    if (i >= s.size() || s[i++] != ':') return false;
                }
                if (!value()) return false;
                ws();
                if (i >= s.size()) return false;
                if (s[i] == ',') { i++; continue; }
                if (s[i] == close) { i++; return true; }
                return false;
            }
        }
        while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}' && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') i++;
        return true;
    }
    /** Moves to the start of member `key` of the object / element `idx` of the array at i. */
    bool enter(const std::variant<size_t, std::string>& step)
    {
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if ((c == '[') != std::holds_alternative<size_t>(step) || (c != '[' && c != '{')) return false;
        i++;
        size_t n = 0;
        while (true) {
            ws();
            if (i < s.size() && (s[i] == ']' || s[i] == '}')) return false;
            if (c == '{') {
                std::string k;
                if (!string(&k)) return false;
                ws();
                if (i >= s.size() || s[i++] != ':') return false;
                ws();
                if (k == std::get<std::string>(step)) return true;
            } else if (n == std::get<size_t>(step)) {
                ws();
                return true;
            }
            if (!value()) return false;
            ws();
            if (i >= s.size() || s[i] != ',') return false;
            i++;
            n++;
        }
    }
};

using Path = std::vector<std::variant<size_t, std::string>>;

bool find_path(const UniValue& v, const UniValue* target, Path& path)
{
    if (&v == target) return true;
    if (v.isArray() || v.isObject()) {
        const auto& vals = v.getValues();
        for (size_t k = 0; k < vals.size(); k++) {
            if (v.isArray()) path.emplace_back(k); else path.emplace_back(v.getKeys()[k]);
            if (find_path(vals[k], target, path)) return true;
            path.pop_back();
        }
    }
    return false;
}

/** serde_json's " at line L column C" for the value at `path` in the params text: the position of its last character. */
std::string position_suffix(std::string_view raw, const Path& path)
{
    RawJson r{raw};
    for (auto& step : path)
        if (!r.enter(step)) return "";
    r.ws();
    size_t start = r.i;
    if (!r.value() || r.i == start) return "";
    size_t last = r.i - 1;
    size_t line = 1, line_start = 0;
    for (size_t k = 0; k < last; k++)
        if (raw[k] == '\n') { line++; line_start = k + 1; }
    return " at line " + std::to_string(line) + " column " + std::to_string(last - line_start);
}

/** The raw text of the "params" member of the request object starting at `obj` (empty if none). */
std::string_view raw_params_of(std::string_view body, size_t obj)
{
    RawJson r{body, obj};
    if (!r.enter(std::string("params"))) return {};
    size_t start = r.i;
    if (!r.value()) return {};
    return body.substr(start, r.i - start);
}
} // namespace

UniValue Server::call(const UniValue& req, const std::function<std::string_view()>& raw_params_of_req)
{
    UniValue id = req["id"];
    if (!req.isObject() || !req["method"].isStr()) {
        auto e = error_obj(-32600, "Invalid request");
        return response(id, nullptr, &e);
    }
    auto it = methods_.find(req["method"].get_str());
    if (it == methods_.end()) {
        auto e = error_obj(-32601, "Method not found");
        return response(id, nullptr, &e);
    }
    Params p;
    const UniValue& params = req["params"];
    auto& m = it->second;
    if (params.isArray()) {
        // jsonrpsee reads the parameters it needs from the sequence and ignores any after them (a
        // method without parameters does not look at them at all).
        p.v = params.getValues();
        if (p.v.size() > m.names.size()) p.v.resize(m.names.size());
    } else if (params.isObject()) {
        for (auto& n : m.names) p.v.push_back(params[n]);
    } else if (!params.isNull() && !m.names.empty()) {
        auto e = error_obj(-32602, "Invalid params");
        return response(id, nullptr, &e);
    }
    for (size_t i = 0; i < m.required; i++) {
        if (i < p.v.size() && !p.v[i].isNull()) continue;
        // A required parameter missing: the sequence ran out ("No more params"), or serde's missing
        // field of a named call.
        auto e = error_obj(-32602, "Invalid params", params.isObject() ? "missing field `" + m.names[i] + "`" : std::string{"No more params"});
        return response(id, nullptr, &e);
    }
    try {
        UniValue r = m.h(p);
        return response(id, &r, nullptr);
    } catch (const RpcError& ex) {
        auto data = ex.data;
        const std::string_view raw_params = data && ex.at && raw_params_of_req ? raw_params_of_req() : std::string_view{};
        if (!raw_params.empty()) {
            Path path;
            bool found = false;
            for (size_t k = 0; k < p.v.size() && !found; k++) {
                if (params.isObject()) path = {m.names[k]}; else path = {k};
                found = find_path(p.v[k], ex.at, path);
            }
            if (found) *data += position_suffix(raw_params, path);
        }
        auto e = error_obj(ex.code, ex.what(), data);
        return response(id, nullptr, &e);
    } catch (const std::exception& ex) {
        auto e = error_obj(-1, ex.what());
        return response(id, nullptr, &e);
    }
}

std::string Server::handle(const std::string& body)
{
    UniValue req;
    if (!req.read(body)) {
        auto e = error_obj(-32700, "Parse error");
        return response(UniValue(), nullptr, &e).write();
    }
    if (req.isArray()) {
        if (req.size() > MAX_BATCH) {
            // jsonrpsee's BATCHES_TOO_LARGE (theirs is unlimited by default; a bound of ours).
            auto e = error_obj(-32010, "The batch request was too large", "Exceeded max limit of " + std::to_string(MAX_BATCH));
            return response(UniValue(), nullptr, &e).write();
        }
        // Where each element starts in the raw text: one pass over the body, done only if a parameter error
        // needs a position (never a rescan per element).
        std::vector<size_t> starts;
        bool scanned = false;
        const auto element_params = [&](size_t k) -> std::string_view {
            if (!scanned) {
                scanned = true;
                RawJson r{body};
                r.ws();
                if (r.i < body.size() && body[r.i] == '[') {
                    r.i++;
                    while (true) {
                        r.ws();
                        if (r.i >= body.size() || body[r.i] == ']') break;
                        starts.push_back(r.i);
                        if (!r.value()) break;
                        r.ws();
                        if (r.i < body.size() && body[r.i] == ',') r.i++; else break;
                    }
                }
            }
            return k < starts.size() ? raw_params_of(body, starts[k]) : std::string_view{};
        };
        UniValue out(UniValue::VARR);
        size_t n = 0;
        for (auto& r : req.getValues()) {
            bool notification = r.isObject() && !r.exists("id");
            const size_t k = n++;
            UniValue resp = call(r, [&] { return element_params(k); });
            if (!notification) out.push_back(resp);
        }
        return out.empty() ? "" : out.write();
    }
    bool notification = req.isObject() && !req.exists("id");
    UniValue resp = call(req, [&] {
        RawJson top{body};
        top.ws();
        return raw_params_of(body, top.i);
    });
    return notification ? "" : resp.write();
}

static bool is_loopback_ip(const std::string& ip) { return ip.rfind("127.", 0) == 0 || ip == "::1" || ip == "localhost"; }

bool Server::listen(const std::string& ip, uint16_t port, std::string& err)
{
    if (!is_loopback_ip(ip) && !options.allow_external) {
        err = "refusing to serve the unauthenticated RPC on non-loopback " + ip + " (pass --bt-rpc-allow-external to override)";
        return false;
    }
    bind_ip_ = ip;
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        err = "bad RPC address " + ip;
        sclose(fd);
        return false;
    }
    if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(fd, 64) != 0) {
        err = "cannot bind RPC " + ip + ":" + std::to_string(port);
        sclose(fd);
        return false;
    }
    fd_ = intptr_t(fd);
    running_ = true;
    thr_ = std::thread([this] {
        while (running_) {
            sock_t c = accept(sock_t(fd_), nullptr, nullptr);
            if (c == sock_t(-1)) {
                if (!running_) break;
                continue;
            }
            std::thread([this, c] { serve(intptr_t(c)); }).detach();
        }
    });
    return true;
}

void Server::stop()
{
    if (!running_.exchange(false)) return;
#ifdef _WIN32
    shutdown(sock_t(fd_), SD_BOTH);
#else
    shutdown(sock_t(fd_), SHUT_RDWR);
#endif
    sclose(sock_t(fd_));
    if (thr_.joinable()) thr_.join();
}

static std::string lower(std::string v)
{
    for (auto& ch : v) ch = char(std::tolower((unsigned char)ch));
    return v;
}

// Reads one request; headers keys lower-cased. Returns false on malformed input.
static bool read_request(sock_t c, std::string& method, std::map<std::string, std::string>& headers, std::string& body, bool& keep_alive,
                         size_t max_body)
{
    std::string buf;
    char tmp[8192];
    size_t hdr_end;
    while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
        auto n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, size_t(n));
        if (buf.size() > 64 * 1024) return false;
    }
    std::string head = buf.substr(0, hdr_end);
    std::istringstream hs(head);
    std::string line;
    std::getline(hs, line);
    method = line.substr(0, line.find(' '));
    keep_alive = line.find("HTTP/1.1") != std::string::npos;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = lower(line.substr(0, colon)), v = line.substr(colon + 1);
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        headers[k] = v;
    }
    if (auto it = headers.find("connection"); it != headers.end()) keep_alive = lower(it->second) != "close";
    size_t clen = 0;
    if (auto it = headers.find("content-length"); it != headers.end()) {
        auto v = parse_u64(it->second);
        if (!v || *v > max_body) return false;
        clen = size_t(*v);
    }
    body = buf.substr(hdr_end + 4);
    while (body.size() < clen) {
        auto n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        body.append(tmp, size_t(n));
    }
    body.resize(clen);
    return true;
}

static void send_all(sock_t c, const std::string& s)
{
    size_t off = 0;
    while (off < s.size()) {
        auto n = send(c, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += size_t(n);
    }
}

static std::string host_name(const std::string& host)
{
    if (!host.empty() && host[0] == '[') {
        auto e = host.find(']');
        return e == std::string::npos ? host : host.substr(1, e - 1);
    }
    auto c = host.rfind(':');
    return lower(c == std::string::npos ? host : host.substr(0, c));
}

static std::string status_line(int code)
{
    switch (code) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 403: return "HTTP/1.1 403 Forbidden\r\n";
    case 405: return "HTTP/1.1 405 Method Not Allowed\r\n";
    case 415: return "HTTP/1.1 415 Unsupported Media Type\r\n";
    default: return "HTTP/1.1 400 Bad Request\r\n";
    }
}

std::string Server::http(const std::string& method, const std::map<std::string, std::string>& headers, const std::string& body)
{
    auto reply = [](int code, const std::string& extra, const std::string& content, const std::string& ctype) {
        std::string r = status_line(code) + extra;
        if (!ctype.empty()) r += "Content-Type: " + ctype + "\r\n";
        r += "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n" + content;
        return r;
    };
    // DNS rebinding: the Host must be ours
    if (auto it = headers.find("host"); it != headers.end()) {
        std::string h = host_name(it->second);
        bool ok = h == "localhost" || h == "127.0.0.1" || h == "::1" || h == lower(bind_ip_) ||
                  std::find(options.allowed_hosts.begin(), options.allowed_hosts.end(), h) != options.allowed_hosts.end();
        if (!ok) return reply(403, "", "", "");
    }
    // CSRF: browsers send Origin; only explicitly allowed origins get through
    std::string cors;
    if (auto it = headers.find("origin"); it != headers.end()) {
        const std::string& o = it->second;
        bool any = std::find(options.allowed_origins.begin(), options.allowed_origins.end(), "*") != options.allowed_origins.end();
        bool listed = std::find(options.allowed_origins.begin(), options.allowed_origins.end(), o) != options.allowed_origins.end();
        if (!any && !listed) return reply(403, "", "", "");
        cors = "Access-Control-Allow-Origin: " + (any ? std::string("*") : o) +
               "\r\nAccess-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: content-type\r\nVary: Origin\r\n";
    }
    if (method == "OPTIONS") return reply(cors.empty() ? 403 : 200, cors, "", "");
    if (method != "POST") return reply(405, cors, "", "");
    auto ct = headers.find("content-type");
    if (ct == headers.end() || lower(ct->second).rfind("application/json", 0) != 0) return reply(415, cors, "", "");
    std::string out = handle(body);
    return reply(200, cors, out, "application/json; charset=utf-8");
}

void Server::serve(intptr_t ci)
{
    sock_t c = sock_t(ci);
    try {
        for (;;) {
            std::string method, body;
            std::map<std::string, std::string> headers;
            bool keep;
            if (!read_request(c, method, headers, body, keep, options.max_body)) {
                send_all(c, status_line(400) + "Content-Length: 0\r\nConnection: close\r\n\r\n");
                break;
            }
            send_all(c, http(method, headers, body));
            if (!keep) break;
        }
    } catch (...) {
        // never let a malformed request take the process down
    }
    sclose(c);
}

UniValue call(const std::string& host, uint16_t port, const std::string& method, const UniValue& params, std::string& err)
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        err = "cannot resolve " + host;
        return {};
    }
    sock_t c = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(c, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        sclose(c);
        err = "cannot connect to " + host + ":" + std::to_string(port);
        return {};
    }
    freeaddrinfo(res);
    UniValue req(UniValue::VOBJ);
    req.pushKV("jsonrpc", "2.0");
    req.pushKV("id", 1);
    req.pushKV("method", method);
    req.pushKV("params", params);
    std::string body = req.write();
    send_all(c, "POST / HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    std::string resp;
    char tmp[8192];
    for (;;) {
        auto n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        resp.append(tmp, size_t(n));
    }
    sclose(c);
    auto p = resp.find("\r\n\r\n");
    if (p == std::string::npos) {
        err = "bad HTTP response";
        return {};
    }
    UniValue v;
    if (!v.read(resp.substr(p + 4))) {
        err = "bad JSON response";
        return {};
    }
    return v;
}

} // namespace bt::jsonrpc
