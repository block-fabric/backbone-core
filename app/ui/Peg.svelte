<script lang="ts">
  // Drivechain (the kit's Peg place): bring ECX in from eCash (a deposit address to pay from any eCash wallet that makes
  // drivechain deposits, or a deposit paid by the wallet of this node's mainchain interface), send it back (a
  // withdrawal: it leaves the chain now and eCash pays it once a bundle with it is accepted), and follow the bundles.
  // Every payment here is asked in the app's trusted dialog.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import QrCode from '../../components/QrCode.svelte';
  import { CHAIN } from '../config';
  import { DROPIN } from '../dropin.config';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { formatCount, shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import { app, toast } from '../../lib/state.svelte';
  import AddressField from './AddressField.svelte';
  import SetupWallet from './SetupWallet.svelte';
  import { confirmed, dropin, sats, satsNumber, type PegInfo, type Withdrawal } from './api';
  import { SPEND } from './spend';
  import { linkState, linkText } from './link';
  import { drop, follow, refresh } from './store.svelte';

  $effect(() => follow());

  const TABS = [
    { id: 'deposit', label: 'Deposit' },
    { id: 'withdraw', label: 'Withdraw' },
    { id: 'bundles', label: 'Bundles' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'deposit');
  const hasSeed = $derived(!!drop.seed?.hasSeed);
  const testNetwork = $derived(['regtest', 'signet'].includes(drop.overview?.chain.network ?? app.info?.network ?? ''));

  let peg = $state<PegInfo | null>(null);
  let error = $state('');
  async function load() {
    try {
      peg = await dropin.peg();
      error = '';
    } catch (e) {
      error = errorText(e);
    }
  }
  $effect(() => {
    void drop.version;
    void load();
  });

  // ---- deposit
  let depositTo = $state<{ address: string; depositAddress: string } | null>(null);
  let depAmount: bigint | null = $state(null);
  let depFee: bigint | null = $state(BigInt(DROPIN.defaultMainFee));
  let depBusy = $state(false);
  let depError = $state('');
  $effect(() => {
    if (hasSeed && !depositTo && tab === 'deposit')
      dropin
        .newAddress()
        .then((a) => (depositTo = a))
        .catch((e) => (depError = errorText(e)));
  });
  async function deposit() {
    if (!depositTo || depAmount === null || depFee === null) return;
    depBusy = true;
    depError = '';
    try {
      const txid = await confirmed<string>(SPEND.deposit, [depositTo.address, satsNumber(depAmount), satsNumber(depFee)]);
      toast('success', t('Deposit sent on eCash'), t('eCash transaction %1: credited on %2 in the first block after eCash mines it.', shortHash(txid, 10), CHAIN.name));
      depAmount = null;
      await refresh();
    } catch (e) {
      if (!declined(e)) depError = errorText(e);
    } finally {
      depBusy = false;
    }
  }

  // ---- withdraw
  let wTo = $state('');
  let wValid = $state(false);
  let wAmount: bigint | null = $state(null);
  let wMainFee: bigint | null = $state(BigInt(DROPIN.defaultMainFee));
  let wFee: bigint | null = $state(BigInt(DROPIN.defaultFee));
  let wBusy = $state(false);
  let wError = $state('');
  const available = $derived(sats(drop.overview?.wallet.available));
  const wTotal = $derived((wAmount ?? 0n) + (wMainFee ?? 0n) + (wFee ?? 0n));
  const wReady = $derived(wValid && wAmount !== null && wAmount > 0n && wMainFee !== null && wFee !== null && wTotal <= available && !wBusy);
  async function withdraw() {
    if (!wReady || wAmount === null || wMainFee === null || wFee === null) return;
    wBusy = true;
    wError = '';
    try {
      await confirmed<string>(SPEND.withdraw, [wTo.trim(), satsNumber(wAmount), satsNumber(wFee), satsNumber(wMainFee)]);
      toast('success', t('Withdrawal asked'), t('It joins the next withdrawal bundle; eCash pays it once the bundle is accepted.'));
      wAmount = null;
      wTo = '';
      await refresh();
      nav.tab = 'bundles';
    } catch (e) {
      if (!declined(e)) wError = errorText(e);
    } finally {
      wBusy = false;
    }
  }

  // ---- merged mining (test networks: a block of this chain on demand)
  let mineBusy = $state(false);
  async function mine() {
    mineBusy = true;
    try {
      await confirmed('mine', []);
      toast('success', t('Bid sent'), t('The next eCash block may take this chain’s block.'));
      await refresh();
    } catch (e) {
      if (!declined(e)) toast('error', t('Merged mining'), errorText(e));
    } finally {
      mineBusy = false;
    }
  }

  function stateText(w: Withdrawal): string {
    switch (w.state) {
      case 'waiting':
        return t('Waiting for a bundle');
      case 'in-pending-bundle':
        return t('In the bundle being made');
      case 'submitted':
        return t('Voted on by eCash’s miners');
      case 'paid':
        return t('Paid on eCash');
      case 'failed':
        return t('Bundle failed: waits for the next');
    }
    return w.state;
  }
  const ls = $derived(linkState(peg?.mainchain));
</script>

<div class="page drop-peg">
  <header class="page-head">
    <h1>{t('Drivechains')}</h1>
    {#if peg}<span class="muted">{t('Slot %1', peg.slot)} · <span class="dot {ls.tone}"></span> eCash {linkText(peg.mainchain)}</span>{/if}
  </header>
  <div class="tabs" role="tablist" aria-label={t('Drivechains')}>
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}

  {#if drop.seed && !hasSeed && tab !== 'bundles'}
    <SetupWallet compact />
  {:else if tab === 'deposit'}
    <div class="layout">
      <section class="panel" aria-labelledby="dep-any">
        <div class="panel-head"><h2 id="dep-any">{t('From any eCash wallet')}</h2></div>
        <div class="panel-body stack">
          <p class="muted">{t('Pay this deposit address from an eCash wallet that makes drivechain deposits (the Backbone app: Sidechains → %1). The coins are credited here in the first block of %1 after eCash mines the deposit.', CHAIN.name)}</p>
          {#if depositTo}
            <div class="qr"><QrCode text={depositTo.depositAddress} size={168} label={t('QR code of the deposit address')} /></div>
            <div class="row copyrow"><span class="mono break">{depositTo.depositAddress}</span><CopyButton text={depositTo.depositAddress} label={t('Copy the deposit address')} /></div>
          {/if}
        </div>
      </section>
      <section class="panel" aria-labelledby="dep-node">
        <div class="panel-head"><h2 id="dep-node">{t('Through this node’s mainchain interface')}</h2></div>
        <form
          class="panel-body stack"
          onsubmit={(e) => {
            e.preventDefault();
            deposit();
          }}
        >
          <p class="muted">{t('Paid on eCash by the wallet of the mainchain interface this node uses (%1: the enforcer’s wallet, or Backbone’s -grpcwallet).', peg?.mainchain?.node ?? '…')}</p>
          <AmountInput id="dep-amount" label={t('Amount')} bind:value={depAmount} required />
          <AmountInput id="dep-fee" label={t('Fee on eCash')} bind:value={depFee} required />
          {#if depError}<p class="error"><Icon name="alert" size={14} /> {depError}</p>{/if}
          <div class="row">
            <button type="submit" class="btn primary" disabled={depBusy || !depositTo || depAmount === null || depAmount <= 0n || depFee === null || ls.tone === 'bad'}><Icon name="down" size={16} /> {t('Review and deposit…')}</button>
          </div>
          {#if ls.tone === 'bad'}<p class="hint warn">{t('The mainchain interface does not answer: no deposit through it now.')}</p>{/if}
        </form>
      </section>
    </div>
  {:else if tab === 'withdraw'}
    <div class="layout">
      <section class="panel" aria-labelledby="w-title">
        <div class="panel-head"><h2 id="w-title">{t('Withdraw to eCash')}</h2></div>
        <form
          class="panel-body stack"
          onsubmit={(e) => {
            e.preventDefault();
            withdraw();
          }}
        >
          <AddressField id="w-to" label={t('To an eCash address')} side="mainchain" bind:value={wTo} bind:valid={wValid} />
          <AmountInput id="w-amount" label={t('Amount')} bind:value={wAmount} required max={available} />
          <AmountInput id="w-mainfee" label={t('Fee on eCash')} bind:value={wMainFee} required title={t('What the withdrawal pays to eCash’s miners in the bundle.')} />
          <AmountInput id="w-fee" label={t('Fee on %1', CHAIN.name)} bind:value={wFee} required />
          <div class="summary"><span>{t('Leaves your balance')}</span><strong><Amount sats={wTotal} /></strong></div>
          <span class="muted small">{t('Available')}: <Amount sats={available} /></span>
          {#if wError}<p class="error"><Icon name="alert" size={14} /> {wError}</p>{/if}
          <div class="row"><button type="submit" class="btn primary" disabled={!wReady}><Icon name="up" size={16} /> {t('Review and withdraw…')}</button></div>
        </form>
      </section>
      <aside class="panel">
        <div class="panel-body">
          <h3>{t('How a withdrawal is paid')}</h3>
          <ol class="steps">
            <li>{t('Your coins leave %1 at once, as a withdrawal output.', CHAIN.name)}</li>
            <li>{t('The chain gathers withdrawals into a bundle and proposes it to eCash.')}</li>
            <li>{t('eCash’s miners vote on it over many blocks; with enough votes, eCash pays every withdrawal of the bundle.')}</li>
            <li>{t('A bundle that fails gives its withdrawals to the next one.')}</li>
          </ol>
        </div>
      </aside>
    </div>
  {:else}
    <div class="layout">
      <section class="panel" aria-labelledby="b-title">
        <div class="panel-head"><h2 id="b-title">{t('Your withdrawals')}</h2></div>
        <div class="panel-body">
          {#if !peg || peg.withdrawals.length === 0}
            <p class="muted">{t('None in flight.')}</p>
          {:else}
            <ul class="wds">
              {#each peg.withdrawals as w (w.txid + w.mainAddress + w.amount)}
                <li>
                  <span class="state {w.state}">{stateText(w)}</span>
                  <span class="mono addr" title={w.mainAddress}>{w.mainAddress}</span>
                  <Amount sats={sats(w.amount)} />
                  {#if w.m6id}<span class="mono faint" title={w.m6id}>{t('bundle')} {shortHash(w.m6id, 8)}</span>{/if}
                </li>
              {/each}
            </ul>
          {/if}
        </div>
      </section>
      <section class="panel" aria-labelledby="c-title">
        <div class="panel-head"><h2 id="c-title">{t('The chain’s bundles')}</h2></div>
        <div class="panel-body">
          <dl class="facts">
            <dt>{t('Being made')}</dt>
            <dd>{#if peg?.pendingBundle}<span class="mono">{shortHash(peg.pendingBundle.m6id, 10)}</span> · {t('%1 withdrawals', peg.pendingBundle.withdrawals)} · <Amount sats={sats(peg.pendingBundle.total)} />{:else}{t('none')}{/if}</dd>
            <dt>{t('Last failed')}</dt><dd>{peg?.lastFailedHeight !== null && peg?.lastFailedHeight !== undefined ? t('at block %1', formatCount(peg.lastFailedHeight)) : t('none')}</dd>
            <dt>{t('Held for %1', CHAIN.name)}</dt><dd><Amount sats={sats(peg?.wealth)} /></dd>
          </dl>
          {#if testNetwork}
            <div class="mine">
              <h3>{t('Merged mining (test networks)')}</h3>
              <p class="muted small">{t('Bid for the next block of %1 to be merge-mined on eCash, paid by the mainchain interface’s wallet.', CHAIN.name)}</p>
              <button type="button" class="btn small" disabled={mineBusy} onclick={mine}><Icon name="merged" size={15} /> {t('Merge-mine a block…')}</button>
            </div>
          {/if}
        </div>
      </section>
    </div>
  {/if}
</div>

<style>
  .drop-peg {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .page-head {
    display: flex;
    align-items: baseline;
    gap: 0.8rem;
  }
  .layout {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(21rem, 1fr));
    gap: 1rem;
    align-items: start;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.85rem;
  }
  .qr {
    padding: 0.5rem;
    background: #fff;
    border-radius: 0.75rem;
    width: max-content;
  }
  .copyrow {
    gap: 0.4rem;
    align-items: center;
  }
  .break {
    word-break: break-all;
  }
  .summary {
    display: flex;
    justify-content: space-between;
    padding: 0.6rem 0.8rem;
    border-radius: 0.6rem;
    background: color-mix(in srgb, var(--accent) 7%, transparent);
  }
  .small {
    font-size: 0.85rem;
  }
  .hint {
    font-size: 0.85rem;
    margin: 0;
  }
  .warn {
    color: var(--warning);
  }
  .error {
    color: var(--negative);
    display: flex;
    gap: 0.35rem;
    align-items: center;
    margin: 0;
  }
  .steps {
    margin: 0;
    padding-inline-start: 1.2rem;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    color: var(--text-muted);
  }
  h3 {
    font-size: 1rem;
    margin: 0 0 0.6rem;
  }
  .wds {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  .wds li {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    gap: 0.3rem 0.8rem;
    align-items: center;
  }
  .wds .addr {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 0.85rem;
  }
  .wds .faint {
    grid-column: 2 / 4;
    font-size: 0.82rem;
  }
  .state {
    font-size: 0.82rem;
    padding: 0.15rem 0.5rem;
    border-radius: 1rem;
    background: color-mix(in srgb, var(--warning) 16%, transparent);
    color: var(--warning);
    width: max-content;
  }
  .state.paid {
    background: color-mix(in srgb, var(--positive) 16%, transparent);
    color: var(--positive);
  }
  .state.failed {
    background: color-mix(in srgb, var(--negative) 14%, transparent);
    color: var(--negative);
  }
  .facts {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.4rem 1rem;
    margin: 0;
  }
  .facts dt {
    color: var(--text-muted);
  }
  .facts dd {
    margin: 0;
  }
  .mine {
    margin-top: 1.2rem;
    padding-top: 1rem;
    border-top: 1px solid var(--border);
  }
  .dot {
    display: inline-block;
    width: 0.55rem;
    height: 0.55rem;
    border-radius: 50%;
    background: var(--text-muted);
  }
  .dot.good {
    background: var(--positive);
  }
  .dot.warn {
    background: var(--warning);
  }
  .dot.bad {
    background: var(--negative);
  }
</style>
