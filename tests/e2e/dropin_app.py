#!/usr/bin/env python3
# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license.
"""The end-to-end run of a drop-in chain's app on a local eCash regtest (doc/app.md "Tests"), shared by the chains.

Backbone (backboned -regtest -ecashregtest=betanet -grpc) is the mainchain, the chain's slot proposed and acked by its
block producer. The app (a build with BT_APP_TEST_HOOKS) runs its node A in a virtual display with the chain's page
script; a second node B (the drop-in, headless, its own wallet) is the other side of the payments. A miner thread
merge-mines a block of the chain (A's LayerTwo `mine`, Backbone's wallet bids) every few seconds. Then the LayerTwo
RPC of A's data is read by a node without the app layer and by one with it: every reply byte-identical, and the app
RPC's reads the LayerTwo results.

Ports: base+10.. (Backbone), base+20.. (A: LayerTwo RPC, P2P, app RPC), base+25.. (B). Every process is stopped by its
PID, never by name.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from dropin_regtest import JsonRpc, Mainchain, Procs, bmm_block, wait_for

HERE = Path(__file__).resolve().parent


@dataclass
class Chain:
    stem: str                  # thunder: the node's binary; <stem>-app the app's
    title: str                 # Thunder: the slot's title, app.info's name
    slot: int
    script: Path               # the chain's page script (joined after common.js)
    reads: list                # LayerTwo reads compared byte for byte: [(method, params)]
    url_flag: str = "--mainchain-grpc-url"
    node_args: list = field(default_factory=list)   # more arguments of every node (BitNames: --bt-no-zmq)
    # A line of the page "SIGNAL <what> ...": the chain's own answer (B pays, B resolves...); returns failures.
    on_signal: Callable[[str, "Run"], list] | None = None
    # Reads with A's own addresses or its tip: [(method, params)] from (a, addresses).
    more_reads: Callable[[JsonRpc, list], list] | None = None


@dataclass
class Run:
    a: JsonRpc
    b: JsonRpc
    main: Mainchain
    base: Path


def main(chain: Chain, argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, type=Path, help=f"folder of {chain.stem} and {chain.stem}-app")
    ap.add_argument("--backboned", required=True, type=Path)
    ap.add_argument("--tmp", required=True, type=Path)
    ap.add_argument("--shots", required=True, type=Path)
    ap.add_argument("--base-port", type=int, default=49800)
    args = ap.parse_args(argv)
    if shutil.which("xvfb-run") is None:
        sys.exit("xvfb-run is needed")
    base = args.tmp.resolve()
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    args.shots.mkdir(parents=True, exist_ok=True)
    P = args.base_port
    procs = Procs(base / "logs")
    main_chain = Mainchain(procs, args.backboned, base / "backbone", P + 10, P + 11, P + 13)
    failures = []
    stop_miner = threading.Event()
    node = args.bin / chain.stem
    try:
        print(f"== mainchain: backboned regtest, slot {chain.slot}", flush=True)
        main_chain.start()
        main_chain.activate(chain.slot, chain.title)
        grpc_url = f"http://127.0.0.1:{P + 13}"

        print("== node B (the other side)", flush=True)
        procs.spawn(f"{chain.stem}B", [node, "--network", "regtest", "-d", base / "B", chain.url_flag, grpc_url, "-r", f"127.0.0.1:{P + 25}",
                                       "-p", f"127.0.0.1:{P + 25}", "-n", f"127.0.0.1:{P + 26}", "--bt-no-seeds", "--add-peer", f"127.0.0.1:{P + 21}",
                                       "--log-level", "INFO"] + chain.node_args)
        b = JsonRpc(P + 25, version="2.0")
        wait_for(lambda: b.getblockcount() is not None, "node B", 60)
        b.set_seed_from_mnemonic(b.generate_mnemonic())
        peer_address = b.get_new_address()

        print(f"== {chain.stem}-app (node A) in a virtual display", flush=True)
        main_address = main_chain.wallet.getnewaddress()
        prelude = f"window.__E2E = {json.dumps({'peerAddress': peer_address, 'mainAddress': main_address})};\n"
        script = base / "app.js"
        script.write_text(prelude + (HERE / "common.js").read_text() + "\n" + chain.script.read_text())
        home = base / "home"
        (home / "run").mkdir(parents=True)
        os.chmod(home / "run", 0o700)
        env = dict(os.environ, HOME=str(home), XDG_RUNTIME_DIR=str(home / "run"), XDG_CONFIG_HOME=str(home / ".config"),
                   LIBGL_ALWAYS_SOFTWARE="1", WEBKIT_DISABLE_COMPOSITING_MODE="1", WEBKIT_DISABLE_DMABUF_RENDERER="1")
        app_cmd = ["xvfb-run", "-a", "-s", "-screen 0 1400x900x24", args.bin / f"{chain.stem}-app", "--network", "regtest", "--datadir", base / "A",
                   "--mainchain-grpc-url", grpc_url, "--rpc-port", str(P + 20), "--p2p-port", str(P + 21), "--app-rpc-port", str(P + 22),
                   "--node-arg=--bt-no-seeds", f"--node-arg=--add-peer=127.0.0.1:{P + 26}"] + [f"--node-arg={x}" for x in chain.node_args] + [
                   "--webuiconfig", base / f"{chain.stem}-app.json", "--webuitestscript", script, "--webuitestshots", args.shots.resolve()]
        log_path = base / "logs" / f"{chain.stem}-app.log"
        app = procs.spawn(f"{chain.stem}-app", app_cmd, env=env)
        a = JsonRpc(P + 20, version="2.0")
        wait_for(lambda: a.getblockcount() is not None, "node A (run by the app)", 120)
        run = Run(a, b, main_chain, base)

        def miner():
            # A block of the chain every few seconds while the run lasts (A's LayerTwo `mine`; Backbone's wallet bids).
            while not stop_miner.is_set():
                try:
                    bmm_block(a, main_chain, lambda: a.getblockcount())
                except Exception as e:  # noqa: BLE001
                    print(f"  miner: {e}", flush=True)
                stop_miner.wait(3)

        threading.Thread(target=miner, daemon=True).start()

        def signals():
            done = set()
            while app.poll() is None:
                text = log_path.read_text(errors="replace") if log_path.exists() else ""
                for line in text.splitlines():
                    # The page's words as the app prints them (its log has them too, with a time: not those).
                    if not line.startswith("webui test: "):
                        continue
                    msg = line[len("webui test: "):]
                    if msg in done:
                        continue
                    done.add(msg)
                    print("  " + msg, flush=True)
                    if msg.startswith("SIGNAL receive-address "):
                        to = msg.split()[-1]
                        wait_for(lambda: b.balance()["available_sats"] >= 50_000_000, "B's coins", 300, 1)
                        txid = b.create_transfer(to, 25_000_000, 1000)
                        print(f"  B pays 0.25 ECX to A: {txid}", flush=True)
                    elif msg.startswith("SIGNAL ") and chain.on_signal:
                        failures.extend(chain.on_signal(msg[len("SIGNAL "):], run))
                time.sleep(0.5)

        signals()
        code = app.returncode
        text = log_path.read_text(errors="replace")
        results = [line.split("RESULT ", 1)[1] for line in text.splitlines() if line.startswith("webui test: RESULT ")]
        if not results:
            failures.append(f"{chain.stem}-app gave no result (exit code {code})")
        else:
            failures += json.loads(results[-1])["failures"]
        if code != 0:
            failures.append(f"{chain.stem}-app exit code {code}")
        stop_miner.set()
        time.sleep(4)
        # The withdrawal paid on eCash: Backbone's wallet received it.
        received = main_chain.wallet.getreceivedbyaddress(main_address, 0)
        print(f"== eCash received {received} ECX from the withdrawal", flush=True)
        if float(received) < 2:
            failures.append(f"the withdrawal's eCash address received {received}")

        print("== the LayerTwo RPC of A's data, without and with the app layer", flush=True)
        procs.procs.pop(f"{chain.stem}-app", None)
        replies = {}
        for mode in ("plain", "app"):
            extra = [] if mode == "plain" else ["--bt-app-rpc-port", str(P + 22)]
            procs.spawn(f"{chain.stem}A-{mode}", [node, "--network", "regtest", "-d", base / "A", chain.url_flag, grpc_url, "-r", f"127.0.0.1:{P + 20}",
                                                  "-p", f"127.0.0.1:{P + 20}", "-n", f"127.0.0.1:{P + 21}", "--bt-no-seeds", "--bt-no-p2p"] + chain.node_args + extra)
            wait_for(lambda: a.getblockcount() is not None, f"A ({mode})", 60)
            addrs = a.get_wallet_addresses()
            calls = chain.reads + [("get_utxos", [addrs]), ("get_stxos", [addrs]), ("get_block_hash", [1]), ("format_deposit_address", [addrs[0]]),
                                   ("get_block", [a.get_best_sidechain_block_hash()]), ("get_bmm_inclusions", [a.get_best_sidechain_block_hash()])]
            if chain.more_reads:
                calls += chain.more_reads(a, addrs)
            replies[mode] = {f"{m} {json.dumps(p)}": a.raw(m, p) for m, p in calls}
            if mode == "app":
                cookie = (base / "A" / "backbone-app.cookie").read_text().split(":", 1)
                appr = JsonRpc(P + 22, cookie[0], cookie[1])
                for m, p in calls:
                    reply = json.loads(a.raw(m, p))
                    if reply.get("error"):
                        continue
                    if appr.call(m, *p) != reply.get("result"):
                        failures.append(f"the app RPC's {m} is not the LayerTwo result")
            procs.stop(f"{chain.stem}A-{mode}")
        same = [k for k in replies["plain"] if replies["plain"][k] == replies["app"][k]]
        print(f"   {len(same)}/{len(replies['plain'])} replies byte-identical", flush=True)
        for k in replies["plain"]:
            if replies["plain"][k] != replies["app"][k]:
                failures.append(f"LayerTwo reply differs with the app layer: {k}")
    except Exception as e:  # noqa: BLE001
        failures.append(f"runner: {e!r}")
    finally:
        stop_miner.set()
        time.sleep(1)
        for name in list(procs.procs):
            if name != "backboned":
                procs.stop(name)
        main_chain.stop()
        procs.stop_all()
    if failures:
        print("FAILED:")
        for f in failures:
            print("  " + f)
        return 1
    print(f"{chain.stem}-app e2e: every step passed")
    return 0
