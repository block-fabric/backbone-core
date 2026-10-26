<script lang="ts">
  // The node (the kit's Node window place): what it is and how it is linked to eCash, and its peers.
  import Icon from '../../components/Icon.svelte';
  import { CHAIN } from '../config';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatCount, formatTime } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import { app, toast } from '../../lib/state.svelte';
  import { host } from '../../lib/rpc';
  import { dropin, type Peer } from './api';
  import { linkText } from './link';
  import { drop, embedded, follow } from './store.svelte';

  $effect(() => follow());

  const TABS = [
    { id: 'info', label: 'Information' },
    { id: 'peers', label: 'Peers' },
  ];
  const tab = $derived(nav.tab === 'peers' ? 'peers' : 'info');
  let peers = $state<Peer[]>([]);
  let address = $state('');
  let error = $state('');
  async function loadPeers() {
    try {
      peers = await dropin.peers();
    } catch (e) {
      error = errorText(e);
    }
  }
  $effect(() => {
    void drop.version;
    if (tab === 'peers') void loadPeers();
  });
  async function connect() {
    try {
      await dropin.connectPeer(address.trim());
      address = '';
      toast('success', t('Connecting'), t('The node dials it.'));
      await loadPeers();
    } catch (e) {
      error = errorText(e);
    }
  }
  async function forget(a: string) {
    try {
      await dropin.forgetPeer(a);
      await loadPeers();
    } catch (e) {
      error = errorText(e);
    }
  }
  const o = $derived(drop.overview);
  const link = $derived(o?.mainchain);
</script>

<div class="page drop-node">
  <header class="page-head"><h1>{t('Node')}</h1></header>
  <div class="tabs" role="tablist" aria-label={t('Node')}>
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}
  {#if tab === 'info'}
    <div class="grid">
      <section class="panel">
        <div class="panel-head"><h2>{CHAIN.name}</h2></div>
        <div class="panel-body">
          <dl class="facts">
            <dt>{t('Version')}</dt><dd>{app.info?.version ?? '–'}</dd>
            <dt>{t('Network')}</dt><dd>{o?.chain.network ?? app.info?.network ?? '–'}</dd>
            <dt>{t('Slot on eCash')}</dt><dd class="num">{o?.chain.slot ?? '–'}</dd>
            <dt>{t('Blocks')}</dt><dd class="num">{o && o.chain.height >= 0 ? formatCount(o.chain.height) : '–'}</dd>
            <dt>{t('Tip')}</dt><dd class="mono break">{o?.chain.tip || '–'}</dd>
            <dt>{t('Peers')}</dt><dd class="num">{o?.chain.peers ?? '–'}</dd>
            {#if app.info?.datadir}<dt>{t('Data folder')}</dt><dd class="mono break">{app.info.datadir}</dd>{/if}
          </dl>
          {#if !embedded()}
            <button type="button" class="btn small" onclick={() => host.open('debuglog').catch((e) => toast('error', t('Log'), errorText(e)))}><Icon name="text" size={15} /> {t('Open the node’s log')}</button>
          {/if}
        </div>
      </section>
      <section class="panel">
        <div class="panel-head"><h2>eCash</h2></div>
        <div class="panel-body">
          <dl class="facts">
            <dt>{t('Link')}</dt><dd>{linkText(link)}</dd>
            <dt>{t('Interface')}</dt><dd class="mono break">{link?.node ?? '–'}</dd>
            <dt>{t('eCash block')}</dt><dd class="num">{link && link.height >= 0 ? formatCount(link.height) : '–'}</dd>
            <dt>{t('Followed up to')}</dt><dd class="num">{link && link.tipheight >= 0 ? formatCount(link.tipheight) : '–'}</dd>
            {#if link?.since}<dt>{t('Since')}</dt><dd>{formatTime(link.since)}</dd>{/if}
            {#if link?.error}<dt>{t('Error')}</dt><dd class="bad">{link.error}</dd>{/if}
          </dl>
          <p class="muted small">{t('The node follows eCash through the bip300301 enforcer’s interface (gRPC), served by an enforcer or by Backbone (-grpc).')}</p>
        </div>
      </section>
    </div>
  {:else}
    <section class="panel">
      <div class="panel-head row">
        <h2 class="grow">{t('Peers')}</h2>
        <form
          class="row"
          onsubmit={(e) => {
            e.preventDefault();
            connect();
          }}
        >
          <input type="text" class="mono" placeholder="host:port" aria-label={t('A peer to connect to')} bind:value={address} />
          <button type="submit" class="btn small" disabled={!address.trim()}><Icon name="plus" size={15} /> {t('Connect')}</button>
        </form>
      </div>
      <div class="panel-body">
        {#if peers.length === 0}<p class="muted">{t('No peer.')}</p>{/if}
        <ul class="peers">
          {#each peers as p (p.address + (p.inbound ? ' in' : ' out'))}
            <li>
              <span class="dot" class:on={p.connected}></span>
              <span class="mono grow">{p.address}</span>
              {#if p.name}<span class="muted">{p.name}</span>{/if}
              <span class="muted">{p.connected ? (p.inbound ? t('connected, inbound') : t('connected')) : t('connecting')}</span>
              <button type="button" class="btn ghost small" onclick={() => forget(p.address)}>{t('Forget')}</button>
            </li>
          {/each}
        </ul>
      </div>
    </section>
  {/if}
</div>

<style>
  .drop-node {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(22rem, 1fr));
    gap: 1rem;
    align-items: start;
  }
  .facts {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.4rem 1.2rem;
    margin: 0 0 1rem;
  }
  .facts dt {
    color: var(--text-muted);
  }
  .facts dd {
    margin: 0;
  }
  .break {
    word-break: break-all;
  }
  .bad {
    color: var(--negative);
  }
  .small {
    font-size: 0.85rem;
  }
  .error {
    color: var(--negative);
    margin: 0;
  }
  .peers {
    list-style: none;
    padding: 0;
    margin: 0;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
  }
  .peers li {
    display: flex;
    gap: 0.7rem;
    align-items: center;
  }
  .dot {
    width: 0.55rem;
    height: 0.55rem;
    border-radius: 50%;
    background: var(--warning);
  }
  .dot.on {
    background: var(--positive);
  }
</style>
