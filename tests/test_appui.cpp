// The app layer of the drop-ins (libs/appui): the app RPC's authentication, whitelist and envelope; the remote host's
// one-time tokens (replay, a changed request, a changed question, another wallet, expiry), describeconfirmation, the
// privileges of host methods, chosen files; the drop-in phrases (BIP-85 vectors).
#include "test.h"

#include <bt/appui/auth.h>
#include <bt/appui/host.h>
#include <bt/appui/phrase.h>
#include <bt/appui/rpcserver.h>
#include <bt/bip32.h>
#include <bt/crypto.h>
#include <bt/fsperm.h>
#include <bt/hd.h>

#include <filesystem>
#include <fstream>
#include <thread>

using namespace bt;
using namespace bt::appui;

namespace {
std::map<std::string, std::string> headers_for(const std::string& user, const std::string& password)
{
    return {{"host", "127.0.0.1:49800"}, {"content-type", "application/json"}, {"authorization", "Basic " + base64_encode(user + ":" + password)}};
}

UniValue body_of(const RpcServer::HttpReply& r)
{
    UniValue v;
    if (!v.read(r.body)) return UniValue{};
    return v;
}

/** A host of a toy chain: one read, one host method that asks, one confirmed method. */
struct Toy {
    int sent{0};
    std::string wallet{"w1"};
    ChainHost chain()
    {
        ChainHost c;
        c.name = "Toy";
        c.stem = "toy";
        c.network = "regtest";
        c.info = [] {
            UniValue i{UniValue::VOBJ};
            i.pushKV("name", "Toy");
            return i;
        };
        c.wallet_id = [this] { return wallet; };
        c.page_reads = {"getcount"};
        c.host["toy.pay"] = [this](const UniValue& p, HostEnv& env) {
            Question q;
            q.title = "Pay";
            q.rows.push_back({"Amount", p["amount"].getValStr(), "amount"});
            if (!env.ask(q).accepted) throw HostError(HOST_CANCELLED, "Cancelled");
            ++sent;
            return UniValue{"paid"};
        };
        c.host["toy.read"] = [](const UniValue&, HostEnv&) { return UniValue{42}; };
        c.privileges["toy.read"] = {"getcount"};
        c.confirmed["send"] = Confirmed{
            [](const UniValue& p) {
                UniValue out{UniValue::VARR};
                for (const UniValue& v : p.getValues()) out.push_back(v);
                return out;
            },
            [](const UniValue& p) {
                Question q;
                q.title = "Send";
                q.rows.push_back({"To", p[0].getValStr(), "address"});
                q.rows.push_back({"Amount", p[1].getValStr(), "amount"});
                return q;
            },
            [this](const UniValue&) {
                ++sent;
                return UniValue{"txid"};
            }};
        return c;
    }
};
} // namespace

TEST(appui_rpcauth_matches_share_rpcauth)
{
    // share/rpcauth/rpcauth.py with salt "cb77f0957de88ff388cf817ddbc7273" and password "foo": its HMAC.
    const RpcAuth made{make_rpcauth("alice", "secret")};
    CHECK(check_password(made, "secret"));
    CHECK(!check_password(made, "secreT"));
    auto parsed{parse_rpcauth(format_rpcauth(made))};
    CHECK(parsed && parsed->user == "alice" && parsed->salt == made.salt && parsed->hash == made.hash);
    CHECK(!parse_rpcauth("alice"));
    CHECK(!parse_rpcauth("alice:salt$short"));
    // HMAC-SHA256(key = the salt as text, msg = the password), as Core's rpcauth.py.
    const Hash32 h{hmac_sha256(ByteSpan{reinterpret_cast<const uint8_t*>("cb77f0957de88ff388cf817ddbc7273"), 31},
                               {ByteSpan{reinterpret_cast<const uint8_t*>("foo"), 3}})};
    RpcAuth a{"bob", "cb77f0957de88ff388cf817ddbc7273", hex_encode(h)};
    CHECK(check_password(a, "foo"));
}

TEST(appui_whitelist_as_core)
{
    Whitelist w;
    CHECK(w.may_call_all("anyone"));
    CHECK(w.add("app:getcount,webuihost"));
    CHECK(w.allowed("app", "getcount"));
    CHECK(!w.allowed("app", "stop"));
    // Once one list is given, users without one may call nothing, unless the default says otherwise.
    CHECK(!w.allowed("other", "getcount"));
    w.set_default(false);
    CHECK(w.allowed("other", "getcount"));
    CHECK(w.may_call_all("other"));
    CHECK(!w.may_call_all("app"));
    // A second list for a user narrows it.
    CHECK(w.add("app:getcount"));
    CHECK(w.allowed("app", "getcount") && !w.allowed("app", "webuihost"));
    CHECK(!w.add("nouser"));
}

TEST(appui_server_auth_whitelist_envelope)
{
    ServerOptions o;
    o.auths.push_back(make_rpcauth("app", "pw"));
    o.auths.push_back(make_rpcauth("root", "rootpw"));
    o.whitelist.add("app:getcount");
    o.whitelist.set_default(false);
    RpcServer s(o);
    s.add("getcount", {}, [](const Call&) { return UniValue{7}; });
    s.add("stop", {}, [](const Call&) { return UniValue{"stopping"}; });
    s.add("echo", {"a", "b"}, [](const Call& c) { return c.params; });
    const std::string v1{R"({"jsonrpc":"1.0","id":"x","method":"getcount","params":[]})"};
    // No credentials, wrong password, a browser, another host: refused.
    CHECK_EQ(s.http("POST", "/", {{"host", "127.0.0.1"}}, v1).status, 401);
    CHECK_EQ(s.http("POST", "/", headers_for("app", "nope"), v1).status, 401);
    auto browser{headers_for("app", "pw")};
    browser["origin"] = "http://localhost:3000";
    CHECK_EQ(s.http("POST", "/", browser, v1).status, 403);
    auto rebound{headers_for("app", "pw")};
    rebound["host"] = "evil.example:80";
    CHECK_EQ(s.http("POST", "/", rebound, v1).status, 403);
    CHECK_EQ(s.http("GET", "/", headers_for("app", "pw"), "").status, 405);
    // Legacy envelope: {result, error, id}.
    auto r{s.http("POST", "/", headers_for("app", "pw"), v1)};
    CHECK_EQ(r.status, 200);
    CHECK_EQ(body_of(r).write(), std::string{R"({"result":7,"error":null,"id":"x"})"});
    // 2.0: {jsonrpc, result, id}; an error at HTTP 200.
    r = s.http("POST", "/", headers_for("root", "rootpw"), R"({"jsonrpc":"2.0","id":2,"method":"nope"})");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(body_of(r)["error"]["code"].getInt<int>(), -32601);
    // The whitelist: 403 for a method not listed, for a whole batch with one not listed.
    CHECK_EQ(s.http("POST", "/", headers_for("app", "pw"), R"({"id":1,"method":"stop"})").status, 403);
    CHECK_EQ(s.http("POST", "/", headers_for("app", "pw"), R"([{"id":1,"method":"getcount"},{"id":2,"method":"stop"}])").status, 403);
    r = s.http("POST", "/", headers_for("root", "rootpw"), R"([{"id":1,"method":"getcount"},{"id":2,"method":"stop"}])");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(body_of(r).size(), size_t{2});
    // Named parameters in the method's order; an unknown name refused; a legacy error is HTTP 500.
    r = s.http("POST", "/", headers_for("root", "rootpw"), R"({"id":1,"method":"echo","params":{"b":2,"a":1}})");
    CHECK_EQ(body_of(r)["result"].write(), std::string{"[1,2]"});
    r = s.http("POST", "/", headers_for("root", "rootpw"), R"({"id":1,"method":"echo","params":{"c":2}})");
    CHECK_EQ(r.status, 500);
    // Another path than / or /wallet/<name>: 404.
    CHECK_EQ(s.http("POST", "/rest", headers_for("root", "rootpw"), v1).status, 404);
    CHECK(wallet_of_uri("/wallet/a%20b") == std::optional<std::string>{"a b"});
}

TEST(appui_server_listens_on_loopback_with_a_cookie)
{
    const std::string dir{(std::filesystem::temp_directory_path() / ("bt-appui-" + random_hex(6))).string()};
    std::filesystem::create_directories(dir);
    ServerOptions o;
    o.cookie_file = dir + "/app.cookie";
    o.bind = "0.0.0.0";
    {
        RpcServer refused(o);
        std::string err;
        CHECK(!refused.listen(err));
    }
    o.bind = "127.0.0.1";
    RpcServer s(o);
    s.add("getcount", {}, [](const Call& c) { return UniValue{c.user}; });
    std::string err;
    CHECK(s.listen(err));
    CHECK(s.port() != 0);
    std::ifstream f(o.cookie_file);
    std::string cookie;
    std::getline(f, cookie);
    CHECK(cookie.rfind("__cookie__:", 0) == 0);
    CHECK_EQ(fsperm::mode_of(o.cookie_file), 0600);
    CHECK_EQ(rpc_call("127.0.0.1", s.port(), "__cookie__", cookie.substr(11), "getcount", UniValue{UniValue::VARR}).get_str(), std::string{"__cookie__"});
    bool refused{false};
    try {
        rpc_call("127.0.0.1", s.port(), "__cookie__", "wrong", "getcount", UniValue{UniValue::VARR});
    } catch (const RpcError&) {
        refused = true;
    }
    CHECK(refused);
    s.stop();
    CHECK(!std::filesystem::exists(o.cookie_file));
    std::filesystem::remove_all(dir);
}

TEST(appui_cookie_owner_only_never_through_a_link)
{
    const std::string dir{(std::filesystem::temp_directory_path() / ("bt-cookie-" + random_hex(6))).string()};
    std::filesystem::create_directories(dir);
    std::string why;
    // Owner-only from its first moment, whatever the umask; written over a stale cookie of an earlier run.
    CHECK(write_secret_file(dir + "/app.cookie", "__cookie__:old", why));
    CHECK_EQ(fsperm::mode_of(dir + "/app.cookie"), 0600);
    CHECK(write_secret_file(dir + "/app.cookie", "__cookie__:new", why));
    std::ifstream f(dir + "/app.cookie");
    std::string text;
    std::getline(f, text);
    CHECK_EQ(text, std::string{"__cookie__:new"});
    // A link at its place (to a file another user would read): refused, and the file it points to is not written.
    { std::ofstream(dir + "/target") << "theirs"; }
    std::filesystem::create_symlink(dir + "/target", dir + "/linked.cookie");
    CHECK(!write_secret_file(dir + "/linked.cookie", "__cookie__:x", why));
    std::ifstream t(dir + "/target");
    std::getline(t, text);
    CHECK_EQ(text, std::string{"theirs"});
    // A folder at its place: refused. A server whose cookie cannot be written does not listen.
    std::filesystem::create_directories(dir + "/dir.cookie");
    CHECK(!write_secret_file(dir + "/dir.cookie", "x", why));
    ServerOptions o;
    o.cookie_file = dir + "/linked.cookie";
    RpcServer s(o);
    std::string err;
    CHECK(!s.listen(err));
    // No temporary file is left behind.
    size_t files{0};
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        (void)e;
        ++files;
    }
    CHECK_EQ(files, size_t{4}); // app.cookie, target, linked.cookie, dir.cookie
    std::filesystem::remove_all(dir);
}

TEST(appui_host_tokens_bind_request_question_wallet_once)
{
    Toy toy;
    ServerOptions o;
    o.whitelist.set_default(false);
    RpcServer s(o);
    s.add("getcount", {}, [](const Call&) { return UniValue{3}; });
    RemoteHost host(toy.chain(), {}, s);
    UniValue req;
    CHECK(req.read(R"({"kind":"host","method":"toy.pay","params":{"amount":"1.5"}})"));
    // First: a question with a token, nothing done.
    UniValue out{host.run(req, "", "", UniValue{}, "app")};
    CHECK(out.exists("confirm"));
    CHECK_EQ(toy.sent, 0);
    const std::string token{out["confirm"]["token"].get_str()};
    CHECK_EQ(out["confirm"]["chain"].get_str(), std::string{"Toy"});
    CHECK_EQ(out["confirm"]["rows"][0]["value"].get_str(), std::string{"1.5"});
    // The same request with the token: it runs, once.
    out = host.run(req, token, "", UniValue{}, "app");
    CHECK_EQ(out["result"].get_str(), std::string{"paid"});
    CHECK_EQ(toy.sent, 1);
    auto refused_with = [&](const UniValue& r, const std::string& t) {
        try {
            host.run(r, t, "", UniValue{}, "app");
        } catch (const RpcError& e) {
            return e.code;
        }
        return 0;
    };
    // Replay: refused.
    CHECK_EQ(refused_with(req, token), int{CONFIRMATION_REFUSED});
    CHECK_EQ(toy.sent, 1);
    // A changed request (another amount) with a token of the first: refused, and the token is gone.
    std::string t2{host.run(req, "", "", UniValue{}, "app")["confirm"]["token"].get_str()};
    UniValue other;
    CHECK(other.read(R"({"kind":"host","method":"toy.pay","params":{"amount":"9"}})"));
    CHECK_EQ(refused_with(other, t2), int{CONFIRMATION_REFUSED});
    CHECK_EQ(refused_with(req, t2), int{CONFIRMATION_REFUSED});
    // Another wallet behind the same request: refused.
    std::string t3{host.run(req, "", "", UniValue{}, "app")["confirm"]["token"].get_str()};
    toy.wallet = "w2";
    CHECK_EQ(refused_with(req, t3), int{CONFIRMATION_REFUSED});
    CHECK_EQ(toy.sent, 1);
    // Keys given twice, unknown fields: refused before anything.
    UniValue dup;
    CHECK(dup.read(R"({"kind":"host","method":"toy.pay","params":{"amount":"1","amount":"2"}})"));
    CHECK(refused_with(dup, "") != 0);
    UniValue extra;
    CHECK(extra.read(R"({"kind":"host","method":"toy.pay","params":{},"also":1})"));
    CHECK(refused_with(extra, "") != 0);
    host.stop();
}

TEST(appui_host_expiry_and_question_change)
{
    Bindings b;
    const auto now{std::chrono::steady_clock::now()};
    const std::string t{b.issue("req", "d1", "w", now)};
    CHECK(b.take(t, "req", "d1", "w", now + TOKEN_EXPIRY + std::chrono::seconds{1}) == Bindings::Check::EXPIRED);
    const std::string t2{b.issue("req", "d1", "w", now)};
    CHECK(b.take(t2, "req", "d2", "w", now) == Bindings::Check::QUESTION_CHANGED);
    CHECK(b.take(t2, "req", "d1", "w", now) == Bindings::Check::UNKNOWN);
    for (int i{0}; i < 100; ++i) b.issue("r", "d", "w", now);
    CHECK_EQ(b.size(), MAX_TOKENS);
    // The digest: a question of other rows is another digest.
    Question q;
    q.title = "Send";
    q.rows.push_back({"Amount", "1", "amount"});
    Question q2{q};
    q2.rows[0].value = "2";
    CHECK(digest("r", q) != digest("r", q2));
    CHECK(digest("r", q) != digest("r2", q));
}

TEST(appui_host_reads_privileges_and_describeconfirmation)
{
    Toy toy;
    ServerOptions o;
    o.whitelist.add("app:webuihost,getcount,toy.pay");
    o.whitelist.add("narrow:webuihost");
    o.whitelist.add("sender:webuihost,describeconfirmation,send");
    o.whitelist.set_default(false);
    RpcServer s(o);
    s.add("getcount", {}, [](const Call&) { return UniValue{3}; });
    s.add("notaread", {}, [](const Call&) { return UniValue{"secret"}; });
    RemoteHost host(toy.chain(), {}, s);
    auto code_of = [&](const char* json, const std::string& user) {
        UniValue r;
        CHECK(r.read(json));
        try {
            host.run(r, "", "", UniValue{}, user);
        } catch (const RpcError& e) {
            return e.code;
        }
        return 0;
    };
    // A read of the pages' policy, by a user that may call it.
    UniValue r;
    CHECK(r.read(R"({"kind":"rpc","method":"getcount","params":[]})"));
    CHECK_EQ(host.run(r, "", "", UniValue{}, "app")["result"].getInt<int>(), 3);
    // A method of the server not in the pages' policy: not found for the pages.
    CHECK_EQ(code_of(R"({"kind":"rpc","method":"notaread","params":[]})", "app"), int{HOST_METHOD_NOT_FOUND});
    // A read the user may not call itself: refused.
    CHECK_EQ(code_of(R"({"kind":"rpc","method":"getcount","params":[]})", "narrow"), int{HOST_NOT_ALLOWED});
    // A host method by its privileges (toy.read needs getcount): the app may, narrow may not.
    CHECK_EQ(code_of(R"({"kind":"host","method":"toy.read","params":{}})", "app"), 0);
    CHECK_EQ(code_of(R"({"kind":"host","method":"toy.read","params":{}})", "narrow"), int{HOST_NOT_ALLOWED});
    // A host method without an entry: only when named.
    CHECK_EQ(code_of(R"({"kind":"host","method":"toy.pay","params":{"amount":"1"}})", "narrow"), int{HOST_NOT_ALLOWED});
    // describeconfirmation: the question, a token, the request to send; never run.
    UniValue params;
    CHECK(params.read(R"(["addr1", 5])"));
    const UniValue d{host.describe("send", params, "sender")};
    CHECK(d["needed"].isTrue());
    CHECK_EQ(toy.sent, 0);
    CHECK_EQ(d["request"]["method"].get_str(), std::string{"rpc.confirmed"});
    const UniValue ran{host.run(d["request"], d["token"].get_str(), "", UniValue{}, "sender")};
    CHECK_EQ(ran["result"].get_str(), std::string{"txid"});
    CHECK_EQ(toy.sent, 1);
    // A user that may not call the confirmed method itself: neither described nor run.
    bool refused{false};
    try {
        host.describe("send", params, "app");
    } catch (const RpcError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK_EQ(code_of(R"({"kind":"host","method":"rpc.confirmed","params":{"method":"send","params":["a",1]}})", "app"), int{HOST_NOT_ALLOWED});
    host.stop();
}

TEST(appui_events_in_order_reset_and_wait)
{
    EventLog log;
    {
        EventHub hub(log);
        hub.emit("tip", UniValue{1}, "tip");
        hub.emit("tip", UniValue{2}, "tip");
        hub.emit("connections", UniValue{3});
        hub.flush();
        hub.stop();
    }
    UniValue a{log.after(0, std::chrono::milliseconds{0})};
    CHECK_EQ(a["seq"].getInt<int>(), 1);
    UniValue batch;
    CHECK(batch.read(a["batches"][0].get_str()));
    CHECK_EQ(batch.size(), size_t{2}); // the second tip replaced the first
    CHECK_EQ(batch[0]["data"].getInt<int>(), 2);
    // Nothing new: waits, then answers empty.
    const auto t0{std::chrono::steady_clock::now()};
    UniValue b{log.after(1, std::chrono::milliseconds{150})};
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds{140});
    CHECK_EQ(b["batches"].size(), size_t{0});
    // A number from another run: reset, all kept given.
    CHECK(log.after(99, std::chrono::milliseconds{0})["reset"].isTrue());
    log.stop();
}

TEST(appui_chosen_files_canonical_once_through_what_was_opened)
{
    const std::string dir{std::filesystem::weakly_canonical(std::filesystem::temp_directory_path() / ("bt-files-" + random_hex(6))).string()};
    std::filesystem::create_directories(dir);
    ChosenFiles files;
    const auto now{std::chrono::steady_clock::now()};
    CHECK(!ChosenFiles::problem("relative/path").empty());
    CHECK(!ChosenFiles::problem(dir + "/../x").empty());
    std::filesystem::create_symlink("/etc/hostname", dir + "/link");
    CHECK(!ChosenFiles::problem(dir + "/link").empty());
    // A new file: made in the folder held, 0600.
    const std::string t{files.grant(dir + "/phrase.txt", true, false, "app", now)};
    CHECK(!files.take(t, "other", now)); // another user's token is not taken
    auto f{files.take(t, "app", now)};
    CHECK(f.has_value());
    ChosenFiles::write(*f, "words");
    CHECK_EQ(fsperm::mode_of(dir + "/phrase.txt"), 0600);
    CHECK(!files.take(t, "app", now)); // once
    // There already: not without overwrite; read through what was opened.
    bool refused{false};
    try {
        files.grant(dir + "/phrase.txt", true, false, "app", now);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    auto r{files.take(files.grant(dir + "/phrase.txt", false, false, "app", now), "app", now)};
    CHECK_EQ(ChosenFiles::read(*r, 100), std::string{"words"});
    // A file made between the grant and the write of a new one: not written over.
    auto n{files.take(files.grant(dir + "/new.txt", true, false, "app", now), "app", now)};
    { std::ofstream(dir + "/new.txt") << "theirs"; }
    refused = false;
    try {
        ChosenFiles::write(*n, "ours");
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    // Expired.
    const std::string late{files.grant(dir + "/late.txt", true, false, "app", now)};
    CHECK(!files.take(late, "app", now + ChosenFiles::EXPIRY + std::chrono::seconds{1}));
    std::filesystem::remove_all(dir);
}

TEST(appui_bip85_vectors_and_dropin_phrases)
{
    // BIP-85's own vector: the master key of the BIP, BIP39 12 English words, index 0.
    auto raw{base58check_decode("xprv9s21ZrQH143K2LBWUUQRFXhucrQqBpKdRRxNVq2zBqsx8HVqFk2uYo8kmbaLLHRdqtQpUm98uKfu3vca1LqdGhUtyoFnCNkfmXRyPXLjbKb")};
    CHECK(raw && raw->size() == 78);
    if (raw && raw->size() == 78) {
        bip32::Xpriv m;
        std::memcpy(m.chaincode.data(), raw->data() + 13, 32);
        std::memcpy(m.secret.data(), raw->data() + 46, 32);
        CHECK_EQ(bip85_phrase_from_master(m, 0, 12), std::string{"girl mad pet galaxy egg matter matrix prison refuse sense ordinary nose"});
        CHECK_EQ(bip85_phrase_from_master(m, 0, 18), std::string{"near account window bike charge season chef number sketch tomorrow excuse sniff circle vital hockey outdoor supply token"});
        CHECK_EQ(bip85_phrase_from_master(m, 0, 24),
                 std::string{"puppy ocean match cereal symbol another shed magic wrap hammer bulb intact gadget divorce twin tonight reason outdoor destroy simple truth cigar social volcano"});
    }
    // The index: SHA-256 of the label, the stem and the network.
    CHECK(dropin_phrase_index("thunder", "betanet") != dropin_phrase_index("bitnames", "betanet"));
    CHECK(dropin_phrase_index("thunder", "betanet") != dropin_phrase_index("thunder", "regtest"));
    CHECK(dropin_phrase_index("thunder", "betanet") < 0x80000000u);
    // From the BIP-39 seed of "abandon ... about": a valid phrase of 12 words, its own per chain and network.
    auto p{hd::parse_mnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about")};
    CHECK(p.has_value());
    const auto seed{hd::mnemonic_to_seed(*p)};
    const std::string thunder{dropin_phrase(seed.span(), "thunder", "betanet")};
    CHECK(hd::parse_mnemonic(thunder).has_value());
    CHECK(thunder != dropin_phrase(seed.span(), "bitnames", "betanet"));
    CHECK_EQ(thunder, bip85_phrase(seed.span(), dropin_phrase_index("thunder", "betanet")));
    // The fingerprint: 16 hex, the same for the same seed, another for another.
    CHECK_EQ(seed_fingerprint(seed.span()).size(), size_t{16});
    CHECK(seed_fingerprint(seed.span()) != seed_fingerprint(hd::mnemonic_to_seed(*hd::parse_mnemonic(thunder)).span()));
}

TEST(appui_node_state_and_events_hide_the_wallet_from_a_caller_without_the_wallet_read)
{
    Toy toy;
    ChainHost c{toy.chain()};
    c.snapshot = [] {
        UniValue s{UniValue::VOBJ};
        s.pushKV("connections", 2);
        UniValue b{UniValue::VOBJ};
        b.pushKV("main", 5);
        s.pushKV("balances", b);
        return s;
    };
    ServerOptions o;
    o.whitelist.add("app:webuihost,webuievents,getblockchaininfo,getbalances");
    o.whitelist.add("chainonly:webuihost,webuievents,getblockchaininfo");
    o.whitelist.add("nothing:webuihost,webuievents");
    o.whitelist.add("walletonly:webuihost,webuievents,getbalances");
    o.whitelist.set_default(false);
    RpcServer s(o);
    RemoteHost host(std::move(c), {}, s);
    UniValue r;
    CHECK(r.read(R"({"kind":"host","method":"node.state","params":{}})"));
    CHECK(host.run(r, "", "", UniValue{}, "app")["result"].exists("balances"));
    const UniValue chain_only{host.run(r, "", "", UniValue{}, "chainonly")["result"]};
    CHECK(!chain_only.exists("balances"));
    CHECK_EQ(chain_only["connections"].getInt<int>(), 2);
    // Without the chain read, node.state is not run at all (it is not an empty privilege).
    bool refused{false};
    try {
        host.run(r, "", "", UniValue{}, "nothing");
    } catch (const RpcError& e) {
        refused = e.code == HOST_NOT_ALLOWED;
    }
    CHECK(refused);
    // The wallet's events reach only a caller of the wallet read.
    host.events().emit("tip", UniValue{1});
    host.events().emit("wallet.balance", UniValue{5});
    host.events().flush();
    auto types = [](const UniValue& ev) {
        std::string out;
        for (const UniValue& b : ev["batches"].getValues()) {
            UniValue list;
            CHECK(list.read(b.get_str()));
            for (const UniValue& e : list.getValues()) out += e["type"].get_str() + ";";
        }
        return out;
    };
    CHECK_EQ(types(host.events_for(0, std::chrono::milliseconds{0}, "app")), std::string{"tip;wallet.balance;"});
    CHECK_EQ(types(host.events_for(0, std::chrono::milliseconds{0}, "chainonly")), std::string{"tip;"});
    CHECK_EQ(types(host.events_for(0, std::chrono::milliseconds{0}, "walletonly")), std::string{"wallet.balance;"});
    CHECK_EQ(types(host.events_for(0, std::chrono::milliseconds{0}, "nothing")), std::string{""});
    // A type nobody listed: to nobody but a caller that may call everything.
    host.events().emit("somethingnew", UniValue{1});
    host.events().flush();
    CHECK(types(host.events_for(1, std::chrono::milliseconds{0}, "app")).find("somethingnew") == std::string::npos);
    CHECK(types(host.events_for(1, std::chrono::milliseconds{0}, "root")).find("somethingnew") != std::string::npos);
    host.stop();
}
