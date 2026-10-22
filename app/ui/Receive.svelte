<script lang="ts">
  // Receive: an address of the wallet to be paid at on this chain (a new one each time, as LayerTwo's wallet gives them),
  // its QR code, and the deposit address to pay from eCash. Its Addresses tab: every address of the wallet and what each
  // received.
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import DataTable from '../../components/DataTable.svelte';
  import Icon from '../../components/Icon.svelte';
  import QrCode from '../../components/QrCode.svelte';
  import { CHAIN } from '../config';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { prefs } from '../../lib/state.svelte';
  import { unitInfo } from '../../lib/units';
  import type { Column } from '../../lib/table';
  import SetupWallet from './SetupWallet.svelte';
  import { dropin, sats } from './api';
  import { drop, follow } from './store.svelte';

  $effect(() => follow());

  type Addr = { address: string; depositAddress: string; balance: number; received: number };
  let current: { address: string; depositAddress: string } | null = $state(null);
  let addresses = $state<Addr[]>([]);
  let error = $state('');
  let busy = $state(false);
  let filter = $state('');
  let selected = $state<string | null>(null);
  let sortKey = $state<string | null>('received');
  let sortDir = $state<'asc' | 'desc'>('desc');

  const TABS = [
    { id: '', label: 'Receive' },
    { id: 'addresses', label: 'Addresses' },
  ];
  const tab = $derived(nav.tab === 'addresses' ? 'addresses' : '');
  const hasSeed = $derived(!!drop.seed?.hasSeed);

  async function fresh() {
    busy = true;
    error = '';
    try {
      current = await dropin.newAddress();
    } catch (e) {
      error = errorText(e);
    } finally {
      busy = false;
    }
  }
  async function loadAddresses() {
    try {
      addresses = await dropin.addresses();
    } catch (e) {
      error = errorText(e);
    }
  }
  $effect(() => {
    if (hasSeed && !current) void fresh();
  });
  $effect(() => {
    void drop.version;
    if (hasSeed && tab === 'addresses') void loadAddresses();
  });

  const columns: Column<Addr>[] = $derived([
    { key: 'address', label: 'Address', value: (r) => r.address, width: 'minmax(14rem, 1fr)' },
    { key: 'received', label: `Received (${unitInfo(prefs.unit).short})`, value: (r) => r.received, width: '11rem', align: 'end' },
    { key: 'balance', label: `Holds (${unitInfo(prefs.unit).short})`, value: (r) => r.balance, width: '11rem', align: 'end' },
  ]);
</script>

<div class="page drop-receive">
  <header class="page-head"><h1>{t('Receive')}</h1></header>
  <div class="tabs" role="tablist" aria-label={t('Receive')}>
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>

  {#if drop.seed && !hasSeed}
    <SetupWallet compact />
  {:else if tab === 'addresses'}
    <div class="row head">
      <h2 class="grow">{t('Addresses of this wallet')}</h2>
      <input type="search" placeholder={t('Search')} aria-label={t('Search the addresses')} bind:value={filter} />
    </div>
    <div class="holder">
      <DataTable rows={addresses} {columns} key={(r) => r.address} {filter} bind:selected bind:sortKey bind:sortDir label={t('Addresses')} empty={t('No address yet')}>
        {#snippet cell(row, column)}
          {#if column.key === 'address'}<span class="mono">{row.address}</span>
          {:else if column.key === 'received'}<Amount sats={sats(row.received)} showUnit={false} />
          {:else}<Amount sats={sats(row.balance)} showUnit={false} />{/if}
        {/snippet}
      </DataTable>
    </div>
  {:else}
    <div class="layout">
      <section class="panel addr" aria-labelledby="addr-title">
        <div class="panel-head"><h2 id="addr-title">{t('Your address on %1', CHAIN.name)}</h2></div>
        <div class="panel-body">
          {#if current}
            <div class="qr"><QrCode text={current.address} size={196} label={t('QR code of the address')} /></div>
            <div class="row copyrow">
              <span class="mono big break">{current.address}</span>
              <CopyButton text={current.address} label={t('Copy the address')} />
            </div>
            <p class="hint">{t('Give it to whoever pays you on %1. Every address is yours for good: a new one each time keeps your payments apart.', CHAIN.name)}</p>
          {:else}
            <p class="muted">{busy ? t('Making an address…') : error}</p>
          {/if}
          <div class="row">
            <button type="button" class="btn" disabled={busy} onclick={fresh}><Icon name="plus" size={16} /> {t('New address')}</button>
          </div>
        </div>
      </section>
      <section class="panel" aria-labelledby="dep-title">
        <div class="panel-head"><h2 id="dep-title">{t('From eCash')}</h2></div>
        <div class="panel-body">
          <p class="muted">{t('To bring ECX from eCash into %1, pay this deposit address from an eCash wallet that makes drivechain deposits (the Backbone app: Sidechains → %1), or deposit through this node on the Drivechain page.', CHAIN.name)}</p>
          {#if current}
            <div class="row copyrow">
              <span class="mono break dep">{current.depositAddress}</span>
              <CopyButton text={current.depositAddress} label={t('Copy the deposit address')} />
            </div>
          {/if}
          <button type="button" class="btn small" onclick={() => go('peg', 'deposit')}><Icon name="down" size={15} /> {t('Deposit…')}</button>
        </div>
      </section>
    </div>
  {/if}
</div>

<style>
  .drop-receive {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    height: 100%;
  }
  .layout {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(20rem, 1fr));
    gap: 1rem;
    align-items: start;
  }
  .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
  .qr {
    display: grid;
    place-items: center;
    padding: 0.6rem;
    background: #fff;
    border-radius: 0.75rem;
    width: max-content;
    align-self: center;
  }
  .copyrow {
    gap: 0.4rem;
    align-items: center;
  }
  .big {
    font-size: 1.05rem;
  }
  .dep {
    font-size: 0.9rem;
  }
  .break {
    word-break: break-all;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.85rem;
    margin: 0;
  }
  .head input {
    width: 14rem;
  }
  .holder {
    flex: 1;
    min-height: 16rem;
  }
</style>
