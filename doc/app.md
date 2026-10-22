# The app of a drop-in chain

The drop-in nodes (Thunder, BitNames, and later zSide, BitAssets, Hivemind) are LayerTwo-Labs compatible: same
consensus, same P2P, same JSON-RPC, byte for byte. This document is the design of the app we put on them: our
sidechain app (the sidechain template's window, trusted dialog and Svelte kit, with pages for the drop-in's own data
model), as a standalone `<stem>-app` and as a pack the unified wallet loads. Consensus and the LayerTwo JSON-RPC are
not touched: the app is an extra layer, off unless the node is started with `--bt-app-rpc-port`.

Each chain's own part is in its repository's `doc/app.md` (its pages, its host methods, its manifest, its tests).

## 1. Architecture: the app is a thin client of its node

```
 <stem>-app  (libs/desktop: the template's window, bridge and trusted dialog, compiled from the template at a pinned commit)
   │  starts the node as its child (fork/exec, own process group, PDEATHSIG; Windows: a kill-on-close Job), its stdin
   │  a pipe: the node exits when the app goes (--bt-app-exit-with-stdin)
   │  app RPC  http://127.0.0.1:<app port>/  Basic auth (a random rpcauth user per run), the manifest as its whitelist
   ▼
 <stem>  the drop-in node, unchanged
   ├─ LayerTwo JSON-RPC (as LayerTwo's: 127.0.0.1:<rpc port>, no auth)          ← L2L tools, unchanged
   └─ app layer (libs/appui), only with --bt-app-rpc-port:
        app RPC: webuihost, describeconfirmation, webuievents, webuichosenfile (the template's remote-host protocol,
                 HOST_API 1), appserverproof, the LayerTwo reads the pages use (the same handlers, the same JSON), a few Core-style reads
                 (getblockchaininfo, getbalances, getnetworkinfo, getmainchaininfo, uptime), the unified wallet's
                 phrase setup (getwalletphraseinfo, setwalletphrase), getappmanifest, stop

 unified wallet: its supervisor starts the same node with --bt-app-* (its own rpcauth user for the pack, the pack's
 whitelist), loads the pack's pages in a sandboxed chains-pack://<stem>/ frame, routes their webuihost calls to the
 node's app RPC, asks each spend in its own trusted dialog (describeconfirmation, then the one-time token)
```

**Why a thin client, not the node linked into the window.**
1. One host, one security model. The unified wallet already speaks the template's remote-host protocol to every
   sidechain daemon (`webuihost` + `describeconfirmation` + one-time tokens + rpcwhitelists). The standalone app
   uses the very same path, so the drop-in pages, the privilege table and the spend checks are written and tested
   once, and the pack is the app's interface built differently, not a second implementation.
2. The node stays the drop-in: the same binary, flags and data directory, runnable headless, its crash not the
   window's. No GTK/WebKit/WebView2 in the node.
3. Least privilege between processes: the window holds no key and no phrase. Secrets stay in the node; a page gets
   only what a host method returns, and the app's user at the node is limited to the manifest.

**"The template is the library."** The window, bridge, trusted dialog, single-instance guard, tray, icons, i18n and
the Svelte kit are the sidechain template's own files, taken by `git archive` from the template repository at the
commit the chain pins (`app/kit.commit`), never copied into our repositories: `cmake/BtDesktopApp.cmake` compiles the
template's `src/webui/*.cpp` as they are, with `libs/desktop/compat` standing for the few Bitcoin Core headers they
include (sync, logging, tinyformat, cleanse, fs, strencodings, threadnames, sha256). The kit has one hook for us
(template branch `dropin-kit`): `src/chain/overrides.ts` (`LOCAL_OVERRIDES`: kit places a chain draws itself;
`KIT_NODE_OPTIONS`: hide Core's node options in Settings). A template change reaches the drop-in apps by bumping the
pin, as it reaches every other sidechain.

The interface of a drop-in app (`cmake/assemble_ui.cmake`): the kit at the pin, the drop-in pages shared by every
drop-in chain (`core/app/ui` → `src/chain/dropin`), the chain's own layer (`app/chain` → `src/chain`) and its
manifest (`app/manifest.json` → `src/chain/manifest.json`), then npm.

## 2. The node's app layer (libs/appui)

| Flag | Meaning |
|---|---|
| `--bt-app-rpc-port <port>` | Turns the app layer on: the app RPC on 127.0.0.1 (loopback only, never another address) |
| `--bt-app-rpcauth <user:salt$hash>` | A user (Core's rpcauth: HMAC-SHA256, key = salt); repeatable |
| `--bt-app-rpcwhitelist <user:m1,m2,...>` | What a user may call (Core's semantics: intersection of repeats) |
| `--bt-app-rpcwhitelistdefault[=0\|1]` | A user without a whitelist: nothing (default 1 when any whitelist is given, as Core) |
| `--bt-app-chooser <user>` | The user whose files chosen by the app may be read/written (`webuichosenfile`) |
| `--bt-app-filedir <dir>` | Where the host's file tokens may point (default: none) |
| `--bt-app-exit-with-stdin` | Exit when stdin closes (the app's lifetime) |

The cookie `<datadir>/backbone-app.cookie` (`__cookie__:<64 hex>`) is made atomically owner-only (mkstemp 0600 /
Windows `CREATE_NEW` with an owner-only DACL, fsync, rename; a link or a folder in its place is refused) and deleted at
shutdown; the cookie user may call everything (the unified wallet's supervisor). The server: HTTP/1.1, Bitcoin Core
JSON-RPC 1.0/2.0 envelopes, batches of at most 1000, a request body of at most 16 MB, no Transfer-Encoding, the Host
header loopback only, any Origin refused (no browser may call it), at most 32 connections; the caller authenticated
from its headers before its body is read (401 after 250 ms and the connection closed), a connection not yet
authenticated held at most 10 s; sockets close-on-exec. `appserverproof [challenge]` answers
hex(HMAC-SHA256(cookie secret, "bt-app-server-proof/v1" 0x00 challenge)): the app checks with the cookie it reads from
the node's data folder that the program on the port it chose is its node, before it shows anything from it.

**Host methods and their privileges.** A user may run a host method when its whitelist names it, or names every
LayerTwo method it exercises (`ChainHost::privileges`, the template's rule). The shared ones:

| Host method | Exercises (privilege) |
|---|---|
| `app.hello`, `app.info`, `rpc.confirmedMethods`, `rpc.confirmed` | nothing (`rpc.confirmed` then needs the confirmed method itself) |
| `node.state` | `getblockchaininfo` (the chain read); its `balances` only with `getbalances` (the wallet read) |
| `dropin.overview` | `balance`, `getblockcount` |
| `dropin.history`, `dropin.peg` | `get_wallet_utxos` (`dropin.peg` also `pending_withdrawal_bundle`) |
| `dropin.newAddress` | `get_new_address` |
| `dropin.addresses`, `dropin.seed.status` | `get_wallet_addresses` |
| `dropin.blocks`, `dropin.block` | `get_block` |
| `dropin.transaction` | `get_transaction` |
| `dropin.address` | `get_utxos`, `get_stxos` |
| `dropin.search` | `get_block`, `get_transaction` |
| `dropin.checkAddress` | nothing (its `mine` only with `get_wallet_addresses`) |
| `dropin.peers`, `.connect`, `.forget` | `list_peers`, `connect_peer`, `forget_peer` |
| `dropin.seed.prepare`, `.create`, `.restore` | `set_seed_from_mnemonic` |
| `dropin.seed.show`, `.save` | only by name (each asks in the trusted dialog) |

Whether an address, a block's output or a transaction is the wallet's (`mine`, `ownAddress`, a name in words) is said
only to a caller of the wallet read (`get_wallet_addresses`). Events go out by an explicit map (`event_privileges`):
the chain's (tip, connections, alert...) to a reader of the chain, `wallet.*` to a reader of the wallet, an unlisted
type to nobody (except a caller of everything).

**Spends: `rpc.confirmed`.** The pages never call a spend directly. They ask `rpc.confirmed {method, params}` with the
LayerTwo method and its parameters in LayerTwo's order (`create_transfer [dest, value_sats, fee_sats]`,
`create_withdrawal [mainchain_address, amount, fee, mainchain_fee]`, `create_deposit`, `create_transfer_many`, `mine`,
and a chain's own, e.g. BitNames' `reserve_bitname`, `register_bitname`). The node describes it (a `Question`: title,
rows, the amounts in ECX), refusing before asking what it cannot pay or parse; the app shows it in its trusted dialog;
a yes gives a token bound to the request, the question and the wallet, valid 5 minutes, used once; then the node runs
the LayerTwo handler with exactly the canonical parameters that were described. In the unified wallet the same
question goes through `describeconfirmation` and the app's own dialog.

**The backup phrase never reaches a page.** `dropin.seed.prepare` makes the words in the node and returns an id;
`dropin.seed.create {id}` asks with the words shown only in the trusted dialog; `dropin.seed.show`/`.save` ask too
(save: only to a file the user chose in the app's chooser). Restoring is the one place words come from the page
(typed into the app's own setup page, sent once to the node, never stored by the app). The test hooks record that
words were shown, never the words.

## 3. One backup phrase (the unified wallet)

The drop-in wallets take a BIP-39 phrase of their own (LayerTwo's derivation from it: Thunder's, plain_bitnames').
The unified wallet derives each drop-in's phrase from the user's one phrase with BIP-85, so one backup restores all:

```
index       = BE32(SHA-256("sidechain/dropin/v1" 0x00 stem 0x00 network)[0..4]) & 0x7fffffff
key         = BIP-32 (secp256k1) private key at m/83696968'/39'/0'/12'/index' of the user's BIP-39 seed
entropy     = HMAC-SHA512(key = "bip-entropy-from-k", msg = key)[0..16]
phrase      = the 12 English BIP-39 words of the entropy
fingerprint = hex(SHA-256("sidechain/dropin/fingerprint/v1" 0x00 bip39_seed_of(phrase))[0..8])
```

`stem` is the chain's id in the app's catalog (`thunder`, `bitnames`, ...), never what a node says; `network` the
drop-in's own network name (`betanet`, `regtest`, `signet`, `forknet`). The label is versioned and product-neutral;
the result is a standard BIP-85 child, so any BIP-85 tool (e.g. Coldcard, Sparrow's) can give a chain's phrase back
from the master phrase and the index. Vectors (master "abandon ×11 about", no passphrase), computed by two
implementations (`tests/test_appui.cpp` and an independent Python one checked against BIP-85's own vector):

| stem, network | index | phrase | fingerprint |
|---|---|---|---|
| thunder, betanet | 1875996695 | skull fragile group drastic stumble bridge pledge end flip girl minor animal | 1de17b5ba7143bfe |
| thunder, regtest | 1199653290 | bless weekend until panic travel tank escape you caught chunk project village | 209bc9789feb3ffb |
| bitnames, betanet | 1487479324 | laptop kitchen pilot rookie crawl settle awful scissors cupboard lumber virtual slab | d6912a9b06cf44f7 |
| bitnames, regtest | 756939817 | toast rebuild airport excite load category oil decrease riot ripple simple begin | 28f49c28f4cfb73e |

The unified wallet (its own cookie user; the pages' user may not call these):
1. `getwalletphraseinfo` → `{hasSeed, phraseKept, fingerprint, from, derivation}`; `from`: `unified` (set by it),
   `app` (made in the app), `restored` (typed in the app), `outside` (set through LayerTwo's own
   `set_seed_from_mnemonic` or its `--mnemonic-seed-phrase-path`), empty without a seed.
2. No seed: `setwalletphrase [phrase]` with the derived phrase → `{fingerprint}`; the wallet checks it is the
   fingerprint of the phrase it derived. The node keeps the phrase only as its wallet's seed (unencrypted at rest, as
   LayerTwo's wallets) and records `from: "unified"`.
3. A seed already: the same fingerprint → the chain is covered by the one phrase. Another → the chain has a phrase of
   its own: the unified wallet shows "this chain has its own backup phrase" and offers the chain's own backup flow
   (as zSide does). `setwalletphrase` refuses to replace a different seed, always.

This is sound because the derived phrase is just a phrase to the drop-in: no consensus or wallet change, LayerTwo's own
derivation below it, and BIP-85's one-wayness: a chain's phrase gives nothing of the master or of another chain's.

## 4. The pack (unified wallet)

`cmake --build <build> --target <stem>-app-pack` → `<build>/pack/<stem>/`: the pages built by the template's
`npm run pack` (served under `/packs/<stem>/`) and `pack.json`:

```json
{ "stem": "thunder", "name": "Thunder", "hostApi": 1, "commit": "<the chain repo's HEAD>", "brand": {...}, "coin": {...},
  "rpc": { "protocol": [...], "reads": [...], "confirmed": [...], "host": [...] },
  "dropin": true, "kit": "<the template commit the pages were built on>" }
```

How the unified wallet runs a drop-in chain:
- Starts `<stem>` with its usual flags (`--network`, `--datadir`, the mainchain URL flag of the manifest:
  `--mainchain-grpc-url` or BitNames' `--bt-mainchain-grpc-url`, `--rpc-addr`/`--private-rpc-addr` on 127.0.0.1) and
  `--bt-app-rpc-port <p> --bt-app-rpcauth <its pack user> --bt-app-rpcwhitelist <pack user>:<protocol + reads +
  confirmed + host of pack.json> --bt-app-rpcwhitelistdefault=0 --bt-app-chooser <pack user> --bt-app-exit-with-stdin`;
  its supervisor uses the cookie for `getwalletphraseinfo`/`setwalletphrase`/`stop`.
- The pack's `rpc.host` leaves out `dropin.seed.restore`: in the unified wallet a phrase is typed only in its own
  window (then `setwalletphrase`), never in a page; the pages, embedded, offer no restore and say where it is.
  `dropin.seed.create`/`.show`/`.save` stay: their words are only in the question, which the unified wallet shows in
  its own trusted dialog (`describeconfirmation`), never in the page.
- `HostPrivileges` of the unified wallet: each `rpc.host` method maps to what the table above says it exercises; a
  pack may call nothing outside its pack.json (`rpc.*`), and never `setwalletphrase`, `stop` or a LayerTwo spend by
  name (only through `rpc.confirmed`).
- `app.info` of the node says `{name, hostApi: 1, embedded: true, remote: true, ...}`; the pack is taken only for
  hostApi 1.

## 5. Security

The template's model, unchanged: loopback-only RPC with rpcauth and a per-user whitelist; every spend described by
the node and asked in a trusted dialog, bound to a one-time token; no secret to a page; the event stream and
ownership flags filtered by privilege; files only those the user chose in the app's chooser (held open from the choice,
`O_NOFOLLOW`, new files `O_EXCL`). Residuals, by design of the drop-in: the LayerTwo JSON-RPC stays as LayerTwo has it
(no auth, every wallet method) on 127.0.0.1, for LayerTwo's tools; any local process of the user can spend through
it, with or without our app. The wallet's seed is unencrypted at rest (LayerTwo's wallets), shown in Settings → Wallet;
a phrase made or restored through the app is also kept in `<datadir>/backbone-app.sqlite` (0600) to be shown again.
In memory, the phrase's copies are wiped on every exit path (`WipeOnExit`), the request texts and HTTP bodies after
use, and a token keeps only its request's hash.

## 6. Tests

- `tests/test_appui.cpp`: auth, whitelist, envelopes, the cookie (0600, a link refused), tokens (replay, a changed
  request, another wallet, expiry), privileges, events per privilege, chosen files, BIP-85 vectors and the drop-in
  vectors above, `node.state` gating.
- Each chain: its LayerTwo RPC byte-identical with and without the app layer (every method, bad and good params), the
  app RPC's reads equal to the LayerTwo results, the phrase flows, ownership gating, its manifest equal to the node's.
- `app/ui/dropin.test.ts` (vitest, in each chain's assembled interface): the overrides, the spends' parameters, no
  words through a page, nothing called outside the manifest.
- `tests/e2e/dropin_app.py` + `common.js`: a local eCash regtest (backboned `-grpc`, the slot proposed and acked by its
  block producer), the app in a virtual display: start, phrase, deposit, send, receive, the chain's own steps,
  withdrawal paid on eCash, screenshots, then the LayerTwo replies byte-identical without and with the app layer.

## 7. Platforms

Linux (GTK 3, WebKitGTK 4.1): built, unit-tested and run end to end. Windows (mingw-w64, WebView2): the nodes and
the apps cross-compile (`-DBT_BUILD_APP=ON -DWEBVIEW2_INCLUDE_DIR=<the WebView2 SDK headers, e.g. a Chains depends
build's include/webview2>`); the app's child is a `CreateProcess` in a kill-on-close Job, the cookie gets an owner-only
DACL; not yet run on Windows. macOS: not built.

## 8. Adding a drop-in chain (zSide, BitAssets, Hivemind)

1. `chains/<stem>/src/appui.cpp`: an `AppLayer` like Thunder's (the shared `bt::appui::dropin` helpers: options,
   link monitor, store, poller), its describers for its spends, its host methods `<stem>.*` with their privileges.
2. `main.cpp`: `bt::appui::take_option` in the flag loop, the app layer started after the LayerTwo servers.
3. `app/manifest.json`, `app/kit.commit`, `app/chain/` (chain.json with eCash naming and the slot, overrides.ts,
   dropin.config.ts, its pages), `chains/<stem>/app/main.cpp`, `bt_desktop_app(<stem>-app ...)` under `BT_BUILD_APP`.
   The manifest also says, when they differ from Thunder's: `program` (the node's program, LayerTwo's name: zSide's
   `thunder-orchard`, Hivemind's `truthcoin`; the app stays `<stem>-app`), `datadirName` (LayerTwo's data folder,
   `<data_dir>/<datadirName>`: the app's default is the node's own), `addressFlags` (`addr`: `--rpc-addr` and
   `--private-rpc-addr` host:port; `host-port`: plain-bitassets' `--rpc-host`/`--rpc-port` and the private pair) and
   `mainchainUrlFlag`; `main.cpp` puts them in the `ChainSpec`. `dropin.config.ts` may name the chain's own spends
   for the shared Send and Drivechains pages (`spend: {transfer, deposit, withdraw}`, LayerTwo's names, the parameters
   in the same order: BitAssets' `transfer`/`withdraw`, zSide's `create_transparent_transfer`) and the words and icon
   of its own kinds of activity rows (`kinds`), `app/ui/spend.ts`.
4. Tests as above; `tests/e2e/run_app.py` giving `dropin_app.Chain` its slot, script, reads and signals, and where it
   differs from Thunder: its `program`, `datadir_flag`, `addresses` (`addr_flags`/`host_port_flags`), node B's
   `peer_setup`/`peer_available`/`peer_pay`, `wallet_addresses` and `base_reads`.
