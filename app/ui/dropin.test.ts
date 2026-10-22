// The drop-in pages (core/app/ui, in src/chain/dropin of a drop-in app's interface), over a pretend node: the kit's
// places drawn by them (chain/overrides.ts), the spends only through rpc.confirmed with the LayerTwo parameters in their
// order, the backup phrase never through the page, and no request outside the chain's manifest (app/manifest.json).
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../../lib/bridge';

const manifest = JSON.parse(readFileSync(join(__dirname, '..', 'manifest.json'), 'utf8')) as { reads: string[]; confirmed: string[]; host: string[] };
/** What the app answers itself (bt/desktop app.cpp) or the template's kit asks of every host. */
const APP_METHODS = new Set(['app.hello', 'app.info', 'app.locale', 'app.languages', 'prefs.get', 'prefs.set', 'node.state', 'rpc.confirmed', 'rpc.confirmedMethods', 'window.state', 'window.setWallet', 'clipboard.read', 'clipboard.write', 'file.choose', 'file.read', 'file.write', 'appnode.get', 'appnode.set', 'appnode.chooseDatadir', 'app.restart', 'app.open', 'app.notify', 'dialog.prepare', 'options.get', 'tray.state']);

let calls: BridgeRequest[] = [];
let answers: Record<string, (r: BridgeRequest) => unknown> = {};

const overview = (hasSeed: boolean) => ({
  wallet: { hasSeed, available: 250_000_000, withdrawing: 0, pendingIn: 0, pendingOut: 0 },
  chain: { height: 42, tip: 'ab'.repeat(32), peers: 2, wealth: 1_000_000_000, slot: 9, network: 'regtest' },
  mainchain: { slot: 9, connected: true, node: 'http://127.0.0.1:50051', height: 120, tipheight: 119, state: 'ok' },
});

beforeEach(() => {
  calls = [];
  answers = {
    'dropin.overview': () => overview(true),
    'dropin.seed.status': () => ({ hasSeed: true, phraseKept: true, from: 'app', fingerprint: '0011223344556677', derivation: 'v0.18.1' }),
    'dropin.history': () => [],
    'dropin.checkAddress': (r) => ({ valid: String((r.params as { address: string }).address).startsWith('4X'), mine: false }),
    'dropin.newAddress': () => ({ address: '4XrMw8bmnhchFspSyP4XRUuB5vaR', depositAddress: 's9_4XrMw8bmnhchFspSyP4XRUuB5vaR_6b2679' }),
    'dropin.peg': () => ({ pendingBundle: null, lastFailedHeight: null, withdrawals: [], wealth: 0, mainchain: overview(true).mainchain, slot: 9 }),
    'rpc.confirmed': () => 'cd'.repeat(32),
    'thunder.throughput': () => ({ blocks: [], mempool: { txs: 0, fees: 0 }, limit: 1000 }),
    'dropin.blocks': () => [],
    'dropin.addresses': () => [],
    'dropin.peers': () => [],
    'appnode.get': () => ({ network: 'regtest', networks: ['betanet', 'regtest'], mainchainUrl: 'http://127.0.0.1:50051', defaultMainchainUrl: 'http://localhost:50051', datadir: '/tmp/x', defaultDatadir: '/tmp/x', rpcPort: 6009, p2pPort: 4009, peers: [], fixedByCommandLine: [], log: '' }),
  };
  setTransport(async (req) => {
    calls.push(req);
    const key = req.kind === 'host' ? (req.method ?? '') : `rpc:${req.method}`;
    const answer = answers[key];
    if (!answer) return null;
    return answer(req);
  });
});

afterEach(() => setTransport(null));

function hostCalls(method: string) {
  return calls.filter((c) => c.kind === 'host' && c.method === method);
}

describe('the drop-in pages', () => {
  it('draw the kit places the chain names (Home, Send...), not the kit pages of a Core node', async () => {
    const { LOCAL_OVERRIDES, KIT_NODE_OPTIONS } = await import('../overrides');
    expect(LOCAL_OVERRIDES).toEqual(expect.arrayContaining(['home', 'send', 'receive', 'transactions', 'peg', 'explorer', 'node']));
    expect(KIT_NODE_OPTIONS).toBe(false);
    const PageView = (await import('../../views/PageView.svelte')).default;
    const { nav } = await import('../../lib/nav.svelte');
    nav.page = 'home';
    const { container } = render(PageView);
    await waitFor(() => expect(container.querySelector('.drop-home')).toBeTruthy());
    // No call of the kit's Core wallet (getbalances is the kit shell's, not the page's).
    expect(calls.some((c) => c.method === 'wallet.transactions' || c.method === 'getwalletinfo')).toBe(false);
  });

  it('send: an address the node checked, an amount, then create_transfer [dest, value, fee] through rpc.confirmed only', async () => {
    const { refresh } = await import('./store.svelte');
    await refresh();
    const Send = (await import('./Send.svelte')).default;
    const { container, getByLabelText } = render(Send);
    await fireEvent.input(getByLabelText('Pay to'), { target: { value: '4XrMw8bmnhchFspSyP4XRUuB5vaR' } });
    await waitFor(() => expect(hostCalls('dropin.checkAddress').length).toBeGreaterThan(0), { timeout: 2000 });
    const amount = container.querySelector('#send-amount') as HTMLInputElement;
    await fireEvent.input(amount, { target: { value: '0.5' } });
    const submit = container.querySelector('button[type="submit"]') as HTMLButtonElement;
    await waitFor(() => expect(submit.disabled).toBe(false), { timeout: 2000 });
    await fireEvent.click(submit);
    await waitFor(() => expect(hostCalls('rpc.confirmed').length).toBe(1));
    const params = hostCalls('rpc.confirmed')[0].params as { method: string; params: unknown[] };
    expect(params.method).toBe('create_transfer');
    expect(params.params).toEqual(['4XrMw8bmnhchFspSyP4XRUuB5vaR', 50_000_000, 1000]);
  });

  it('the backup phrase: made by the node under an id, its words never reach the page', async () => {
    answers['dropin.seed.status'] = () => ({ hasSeed: false, phraseKept: false, from: '', fingerprint: '', derivation: 'v0.18.1' });
    answers['dropin.overview'] = () => overview(false);
    answers['dropin.seed.prepare'] = () => ({ id: 'ff'.repeat(16), words: 12 });
    answers['dropin.seed.create'] = () => ({ fingerprint: '0011223344556677' });
    const Setup = (await import('./SetupWallet.svelte')).default;
    const { getByText } = render(Setup);
    await fireEvent.click(getByText('Make a new backup phrase'));
    await waitFor(() => expect(hostCalls('dropin.seed.create').length).toBe(1));
    expect(hostCalls('dropin.seed.create')[0].params).toEqual({ id: 'ff'.repeat(16) });
    // Nothing the page sent or got carries words.
    expect(JSON.stringify(calls)).not.toMatch(/abandon|zoo/);
  });

  it('withdraw: create_withdrawal [eCash address, amount, fee, eCash fee], the address checked as an eCash one', async () => {
    answers['dropin.checkAddress'] = (r) => ({ valid: (r.params as { side?: string }).side === 'mainchain' });
    const { refresh } = await import('./store.svelte');
    await refresh();
    const { nav } = await import('../../lib/nav.svelte');
    nav.tab = 'withdraw';
    const Peg = (await import('./Peg.svelte')).default;
    const { container, getByLabelText } = render(Peg);
    await fireEvent.input(getByLabelText('To an eCash address'), { target: { value: 'bcrt1qexample' } });
    await waitFor(() => expect(hostCalls('dropin.checkAddress').some((c) => (c.params as { side?: string }).side === 'mainchain')).toBe(true), { timeout: 2000 });
    await fireEvent.input(container.querySelector('#w-amount') as HTMLInputElement, { target: { value: '1' } });
    const submit = [...container.querySelectorAll('button[type="submit"]')].pop() as HTMLButtonElement;
    await waitFor(() => expect(submit.disabled).toBe(false), { timeout: 2000 });
    await fireEvent.click(submit);
    await waitFor(() => expect(hostCalls('rpc.confirmed').length).toBe(1));
    const p = hostCalls('rpc.confirmed')[0].params as { method: string; params: unknown[] };
    expect(p.method).toBe('create_withdrawal');
    expect(p.params).toEqual(['bcrt1qexample', 100_000_000, 1000, 1000]);
  });

  it('call nothing of the node outside the manifest (the least the app’s user is given)', async () => {
    for (const page of ['Home', 'Overview', 'Send', 'Receive', 'History', 'Peg', 'Explorer', 'NodePage', 'WalletSettings', 'NodeSettings']) {
      const C = (await import(`./${page}.svelte`)).default;
      render(C);
    }
    await new Promise((r) => setTimeout(r, 400));
    const allowed = new Set([...manifest.host, ...APP_METHODS]);
    const bad = calls.filter((c) => (c.kind === 'host' ? !allowed.has(c.method ?? '') && !(c.method ?? '').startsWith('test.') : !manifest.reads.includes(c.method ?? '')));
    expect(bad.map((c) => `${c.kind}:${c.method}`)).toEqual([]);
    // And every confirmed spend of the pages is one of the manifest's.
    for (const c of hostCalls('rpc.confirmed')) expect(manifest.confirmed).toContain((c.params as { method: string }).method);
  });

  it('words of the activity and of the link', async () => {
    const { kindText, counterparty } = await import('./kinds');
    expect(kindText({ kind: 'deposit' })).toBe('Deposit from eCash');
    expect(kindText({ kind: 'bundle', bundleStatus: 'Confirmed' })).toBe('Paid on eCash');
    expect(counterparty({ kind: 'send', to: [{ address: 'a', amount: 1 }], recipients: 1 } as never)).toBe('a');
    expect(counterparty({ kind: 'send', to: [], recipients: 3 } as never)).toBe('3 recipients');
    const { linkText, linkState } = await import('./link');
    expect(linkState({ state: 'down', connected: false }).tone).toBe('bad');
    expect(linkText({ state: 'syncing', connected: true, behind_blocks: 4 })).toBe('catching up (4 blocks)');
    const { sats, satsNumber } = await import('./api');
    expect(sats(150)).toBe(150n);
    expect(satsNumber(150n)).toBe(150);
    expect(() => satsNumber(-1n)).toThrow();
  });
});
