#!/usr/bin/env python3
# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license.
"""A local eCash regtest for the end-to-end tests of a drop-in chain and its app (doc/app.md).

Backbone (backboned -regtest -ecashregtest=betanet -grpc) is the mainchain and serves the enforcer's interface
(cusf.mainchain.v1, Connect JSON over HTTP/1.1 next to gRPC), its wallet "miner" paying deposits and BMM bids. The
drop-in's slot is proposed and acked by Backbone's own block producer. Every process is started here with its own
data directory and ports, and stopped by its exact PID (never by name). Standard library only.
"""

import base64
import json
import os
import signal
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

PACKAGE = "cusf.mainchain.v1."


def wait_for(fn, what, timeout=120, every=0.5):
    until = time.time() + timeout
    last = None
    while time.time() < until:
        try:
            last = fn()
            if last:
                return last
        except Exception as e:  # noqa: BLE001 - what failed is said at the end
            last = e
        time.sleep(every)
    raise TimeoutError(f"timed out waiting for {what} (last: {last!r})")


def port_free(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


class Procs:
    """The processes of a test, each stopped by its own PID."""

    def __init__(self, logs: Path):
        self.logs = logs
        self.logs.mkdir(parents=True, exist_ok=True)
        self.procs = {}

    def spawn(self, name, cmd, env=None, stdin=None):
        log = open(self.logs / f"{name}.log", "a", encoding="utf-8")
        log.write(f"== {' '.join(str(c) for c in cmd)}\n")
        log.flush()
        p = subprocess.Popen([str(c) for c in cmd], stdout=log, stderr=subprocess.STDOUT, env=env, stdin=stdin or subprocess.DEVNULL, start_new_session=True)
        self.procs[name] = p
        return p

    def stop(self, name, timeout=60):
        p = self.procs.pop(name, None)
        if p is None or p.poll() is not None:
            return
        # Its own process group (start_new_session): what it started with it goes too (xvfb-run's app and the app's node).
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()

    def stop_all(self):
        for name in list(self.procs):
            self.stop(name)


class JsonRpc:
    """A JSON-RPC client: Bitcoin Core's (user/password, 1.0) or LayerTwo's (no auth, 2.0)."""

    def __init__(self, port, user=None, password=None, version="1.0", path="/"):
        self.url = f"http://127.0.0.1:{port}{path}"
        self.auth = base64.b64encode(f"{user}:{password}".encode()).decode() if user else None
        self.version = version

    def raw(self, method, params=None, timeout=120):
        body = json.dumps({"jsonrpc": self.version, "id": 1, "method": method, "params": params if params is not None else []}).encode()
        headers = {"Content-Type": "application/json"}
        if self.auth:
            headers["Authorization"] = "Basic " + self.auth
        req = urllib.request.Request(self.url, body, headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            return e.read()

    def call(self, method, *params, timeout=120):
        reply = json.loads(self.raw(method, list(params), timeout))
        if reply.get("error"):
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply.get("result")

    def __getattr__(self, method):
        if method.startswith("_"):
            raise AttributeError(method)
        return lambda *params, **kw: self.call(method, *params, **kw)


def grpc(port, method, request=None, timeout=60):
    """A unary call of the enforcer's interface, Connect protocol with JSON."""
    if "." not in method.split("/")[0]:
        method = PACKAGE + method
    req = urllib.request.Request(f"http://127.0.0.1:{port}/{method}", json.dumps(request or {}).encode(),
                                 {"Content-Type": "application/json", "Connect-Protocol-Version": "1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = r.read()
            return json.loads(data) if data else {}
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"{method}: {e.code} {e.read()[:300]!r}") from None


class Mainchain:
    """backboned on eCash's regtest, alone, its gRPC the drop-in's mainchain interface."""

    def __init__(self, procs: Procs, binary: Path, datadir: Path, rpc_port: int, p2p_port: int, grpc_port: int):
        self.procs, self.binary, self.datadir = procs, binary, datadir
        self.rpc_port, self.p2p_port, self.grpc_port = rpc_port, p2p_port, grpc_port
        self.rpc = JsonRpc(rpc_port, "e2e", "e2e")
        self.wallet = JsonRpc(rpc_port, "e2e", "e2e", path="/wallet/miner")
        self.lock = threading.Lock()

    def start(self):
        for p in (self.rpc_port, self.p2p_port, self.grpc_port):
            if not port_free(p):
                raise RuntimeError(f"port {p} is taken: another test runs?")
        self.datadir.mkdir(parents=True, exist_ok=True)
        self.procs.spawn("backboned", [self.binary, "-regtest", "-ecashregtest=betanet", f"-datadir={self.datadir}", f"-rpcport={self.rpc_port}",
                                       f"-port={self.p2p_port}", "-rpcuser=e2e", "-rpcpassword=e2e", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
                                       "-grpc", f"-grpcport={self.grpc_port}", "-grpcwallet=miner", "-fallbackfee=0.0002", "-printtoconsole=0"])
        wait_for(lambda: self.rpc.getblockcount() >= 0, "backboned", 180)
        wallets = self.rpc.listwallets()
        if "miner" not in wallets:
            try:
                self.rpc.loadwallet("miner")
            except RuntimeError:
                self.rpc.createwallet("miner")
        self.address = self.wallet.getnewaddress()
        if self.rpc.getblockcount() < 110:
            self.generate(110 - self.rpc.getblockcount())
        wait_for(lambda: grpc(self.grpc_port, "ValidatorService/GetChainTip"), "backboned's gRPC", 60)

    def generate(self, n=1):
        with self.lock:
            return self.rpc.generatetoaddress(n, self.address)

    def activate(self, slot, title):
        active = lambda: any(s.get("slot") == slot for s in self.rpc.listactivesidechains())  # noqa: E731
        if active():
            return
        grpc(self.grpc_port, "BlockProducerService/SetAckAllProposals", {"policy": "ACK_ALL_PROPOSALS_POLICY_ALL"})
        grpc(self.grpc_port, "BlockProducerService/SubmitSidechainProposal",
             {"sidechainId": slot, "declaration": {"v0": {"title": title, "description": f"{title} (drop-in e2e)", "hashId1": {"hex": "11" * 32}, "hashId2": {"hex": "22" * 20}}}})
        for _ in range(60):
            if active():
                return
            self.generate(1)
        raise RuntimeError(f"slot {slot} did not activate")

    def stop(self):
        try:
            self.rpc.stop()
        except Exception:  # noqa: BLE001 - stopped by its PID next
            pass
        p = self.procs.procs.get("backboned")
        if p:
            try:
                p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                pass
        self.procs.stop("backboned")


def bmm_block(l2l: JsonRpc, main: Mainchain, height_of, timeout=60):
    """One block of the drop-in chain: its `mine` (a BMM request through the mainchain interface's wallet, then the
    first event of a new subscription), and the mainchain block that takes the request."""
    before = height_of()
    errors = []

    def mine():
        try:
            l2l.call("mine", None, timeout=timeout)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    t = threading.Thread(target=mine, daemon=True)
    t.start()
    # The request in the mempool first, then the block (the subscription takes the first event after it starts).
    time.sleep(1.5)
    main.generate(1)
    t.join(timeout)
    try:
        wait_for(lambda: height_of() > before, "a block of the drop-in chain", 20, 0.25)
        return True
    except TimeoutError:
        return False
