<script lang="ts">
  // The block explorer of this chain: its latest blocks, a search (a height, a block or transaction hash, an address or
  // a deposit address), and the details of a block, a transaction or an address, read from this node.
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import DataTable from '../../components/DataTable.svelte';
  import Icon from '../../components/Icon.svelte';
  import Meter from '../../components/Meter.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatBytes, formatCount, formatTime, shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav, takeFocus } from '../../lib/nav.svelte';
  import { prefs } from '../../lib/state.svelte';
  import { unitInfo } from '../../lib/units';
  import type { Column } from '../../lib/table';
  import { dropin, sats, type AddressDetail, type BlockDetail, type BlockSummary, type TxDetail } from './api';
  import { drop, follow } from './store.svelte';

  $effect(() => follow());

  type View = { kind: 'list' } | { kind: 'block'; detail: BlockDetail } | { kind: 'tx'; detail: TxDetail } | { kind: 'address'; detail: AddressDetail };
  let view = $state<View>({ kind: 'list' });
  let blocks = $state<BlockSummary[]>([]);
  let query = $state('');
  let error = $state('');
  let selected = $state<string | null>(null);
  let sortKey = $state<string | null>('height');
  let sortDir = $state<'asc' | 'desc'>('desc');
  const PAGE = 25;
  let from = $state<number | undefined>(undefined);

  async function loadBlocks() {
    try {
      blocks = await dropin.blocks(PAGE, from);
      error = '';
    } catch (e) {
      error = errorText(e);
    }
  }
  $effect(() => {
    void drop.version;
    void from;
    if (view.kind === 'list') void loadBlocks();
  });

  async function openBlock(hash: string) {
    try {
      view = { kind: 'block', detail: await dropin.block({ hash }) };
      error = '';
    } catch (e) {
      error = errorText(e);
    }
  }
  async function openTx(txid: string) {
    try {
      view = { kind: 'tx', detail: await dropin.transaction(txid) };
      error = '';
    } catch (e) {
      error = errorText(e);
    }
  }
  async function openAddress(address: string) {
    try {
      view = { kind: 'address', detail: await dropin.address(address) };
      error = '';
    } catch (e) {
      error = errorText(e);
    }
  }
  async function search() {
    const q = query.trim();
    if (!q) return;
    try {
      const r = await dropin.search(q);
      if (r.kind === 'block' && r.hash) await openBlock(r.hash);
      else if (r.kind === 'transaction' && r.txid) await openTx(r.txid);
      else if (r.kind === 'address' && r.address) await openAddress(r.address);
      else error = t('Nothing of this chain is called %1', q);
    } catch (e) {
      error = errorText(e);
    }
  }
  // Opened for a block (the activity's "Open its block").
  $effect(() => {
    const focus = takeFocus('explorer');
    if (focus) void openBlock(focus);
  });

  const columns: Column<BlockSummary>[] = $derived([
    { key: 'height', label: 'Height', value: (b) => b.height, width: '7rem', align: 'end' },
    { key: 'time', label: 'Mined', value: (b) => b.time ?? 0, width: '11rem' },
    { key: 'hash', label: 'Hash', value: (b) => b.hash, width: 'minmax(10rem, 1fr)', mono: true },
    { key: 'txs', label: 'Transactions', value: (b) => b.txs, width: '8rem', align: 'end' },
    { key: 'fill', label: 'Size', value: (b) => b.bytes, width: '10rem' },
    { key: 'fees', label: `Fees (${unitInfo(prefs.unit).short})`, value: (b) => b.fees, width: '10rem', align: 'end' },
  ]);
  const tip = $derived(drop.overview?.chain.height ?? -1);
  void nav;
</script>

<div class="page drop-explorer">
  <header class="page-head row">
    <h1 class="grow">{t('Block explorer')}</h1>
    <form
      class="row search"
      onsubmit={(e) => {
        e.preventDefault();
        search();
      }}
    >
      <input type="search" class="mono" placeholder={t('Height, hash, address')} aria-label={t('Search the chain')} bind:value={query} />
      <button type="submit" class="btn small"><Icon name="search" size={15} /> {t('Search')}</button>
    </form>
  </header>
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}

  {#if view.kind !== 'list'}
    <div><button type="button" class="btn ghost small back" onclick={() => (view = { kind: 'list' })}><Icon name="arrow" size={15} /> {t('Latest blocks')}</button></div>
  {/if}

  {#if view.kind === 'list'}
    <div class="holder">
      <DataTable rows={blocks} {columns} key={(b) => b.hash} bind:selected bind:sortKey bind:sortDir label={t('Blocks')} empty={t('No block yet')} onactivate={(b) => openBlock(b.hash)}>
        {#snippet cell(row, column)}
          {#if column.key === 'height'}<span class="num">{formatCount(row.height)}</span>
          {:else if column.key === 'time'}{row.time ? formatTime(row.time) : '–'}
          {:else if column.key === 'hash'}<span class="mono">{shortHash(row.hash, 14)}</span>
          {:else if column.key === 'txs'}<span class="num">{formatCount(row.txs)}</span>
          {:else if column.key === 'fill'}{#if row.limit > 0}<Meter value={row.bytes} max={row.limit} label={t('How full')} text={formatBytes(row.bytes)} tone={row.bytes > row.limit * 0.8 ? 'warning' : 'accent'} />{:else}<span class="num">{formatBytes(row.bytes)}</span>{/if}
          {:else}<Amount sats={sats(row.fees)} showUnit={false} />{/if}
        {/snippet}
      </DataTable>
    </div>
    <div class="row pager">
      <button type="button" class="btn small" disabled={from === undefined} onclick={() => (from = from === undefined || from + PAGE >= tip ? undefined : from + PAGE)}>{t('Newer')}</button>
      <button type="button" class="btn small" disabled={blocks.length < PAGE || blocks[blocks.length - 1]?.height === 0} onclick={() => (from = (blocks[blocks.length - 1]?.height ?? 1) - 1)}>{t('Older')}</button>
    </div>
  {:else if view.kind === 'block'}
    {@const b = view.detail}
    <section class="panel">
      <div class="panel-head"><h2>{t('Block %1', formatCount(b.height))} {#if !b.onChain}<span class="badge warning">{t('not on the chain')}</span>{/if}</h2></div>
      <div class="panel-body">
        <dl class="facts">
          <dt>{t('Hash')}</dt><dd class="mono break">{b.hash} <CopyButton text={b.hash} /></dd>
          <dt>{t('Previous')}</dt><dd class="mono break">{#if b.prevHash}<button type="button" class="link mono" onclick={() => openBlock(b.prevHash)}>{b.prevHash}</button>{:else}{t('none (the first block)')}{/if}</dd>
          <dt>{t('Merge-mined in eCash block')}</dt><dd class="mono break">{b.mainHash}</dd>
          {#if b.time}<dt>{t('Time')}</dt><dd>{formatTime(b.time)}</dd>{/if}
          <dt>{t('Transactions')}</dt><dd class="num">{formatCount(b.txs)}</dd>
          <dt>{t('Size')}</dt><dd>{formatBytes(b.bytes)}{#if b.limit > 0} / {formatBytes(b.limit)}{/if}</dd>
          <dt>{t('Fees')}</dt><dd><Amount sats={sats(b.fees)} /></dd>
        </dl>
        {#if b.coinbase.length}
          <h3>{t('Paid to the block’s maker')}</h3>
          <ul class="outs">{#each b.coinbase as o, i (i)}<li><button type="button" class="link mono" onclick={() => openAddress(o.address)}>{o.address}</button><Amount sats={sats(o.amount)} /></li>{/each}</ul>
        {/if}
        <h3>{t('Transactions')}</h3>
        {#if b.transactions.length === 0}<p class="muted">{t('None')}</p>{/if}
        <ul class="txs">
          {#each b.transactions as tx (tx.txid)}
            <li>
              <button type="button" class="link mono" onclick={() => openTx(tx.txid)}>{tx.txid}</button>
              <span class="muted">{t('%1 inputs, %2 outputs', tx.inputs, tx.outputs.length)}</span>
              <Amount sats={sats(tx.outputs.reduce((s, o) => s + o.amount, 0))} />
            </li>
          {/each}
        </ul>
      </div>
    </section>
  {:else if view.kind === 'tx'}
    {@const tx = view.detail}
    <section class="panel">
      <div class="panel-head"><h2>{t('Transaction')} {#if tx.pending}<span class="badge warning">{t('waiting for a block')}</span>{/if}</h2></div>
      <div class="panel-body">
        <dl class="facts">
          <dt>{t('Id')}</dt><dd class="mono break">{tx.txid} <CopyButton text={tx.txid} /></dd>
          {#if tx.block}<dt>{t('Block')}</dt><dd><button type="button" class="link mono" onclick={() => openBlock(tx.block ?? '')}>{tx.height !== undefined ? formatCount(tx.height) : shortHash(tx.block, 12)}</button>{#if tx.confirmations} · {t('%1 confirmations', formatCount(tx.confirmations))}{/if}</dd>{/if}
          {#if tx.time}<dt>{t('Time')}</dt><dd>{formatTime(tx.time)}</dd>{/if}
          {#if tx.fee !== undefined}<dt>{t('Fee')}</dt><dd><Amount sats={sats(tx.fee)} /></dd>{/if}
          <dt>{t('Inputs')}</dt><dd class="num">{tx.inputs.length}</dd>
        </dl>
        <h3>{t('Outputs')}</h3>
        <ul class="outs">
          {#each tx.outputs as o, i (i)}
            <li>
              <button type="button" class="link mono" onclick={() => openAddress(o.address)}>{o.address}</button>
              {#if o.mine}<span class="badge">{t('yours')}</span>{/if}
              {#if o.kind === 'withdrawal'}<span class="badge warning" title={o.mainAddress}>{t('withdrawal to %1', shortHash(o.mainAddress ?? '', 10))}</span>{/if}
              <Amount sats={sats(o.amount)} />
            </li>
          {/each}
        </ul>
      </div>
    </section>
  {:else}
    {@const a = view.detail}
    <section class="panel">
      <div class="panel-head"><h2>{t('Address')} {#if a.mine}<span class="badge">{t('yours')}</span>{/if}</h2></div>
      <div class="panel-body">
        <dl class="facts">
          <dt>{t('Address')}</dt><dd class="mono break">{a.address} <CopyButton text={a.address} /></dd>
          <dt>{t('Deposit address')}</dt><dd class="mono break">{a.depositAddress}</dd>
          <dt>{t('Holds')}</dt><dd><Amount sats={sats(a.balance)} /></dd>
          <dt>{t('Outputs held')}</dt><dd class="num">{a.utxos.length}</dd>
          <dt>{t('Outputs spent')}</dt><dd class="num">{a.spent.length}</dd>
        </dl>
      </div>
    </section>
  {/if}
</div>

<style>
  .drop-explorer {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
    height: 100%;
  }
  .search input {
    width: 22rem;
  }
  .holder {
    flex: 1;
    min-height: 18rem;
  }
  .back :global(svg) {
    transform: scaleX(-1);
  }
  .pager {
    justify-content: flex-end;
    gap: 0.5rem;
  }
  .error {
    color: var(--negative);
    margin: 0;
    display: flex;
    gap: 0.35rem;
    align-items: center;
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
  h3 {
    font-size: 0.95rem;
    margin: 0.6rem 0;
  }
  .outs,
  .txs {
    list-style: none;
    padding: 0;
    margin: 0;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .outs li,
  .txs li {
    display: flex;
    gap: 0.6rem;
    align-items: center;
    justify-content: space-between;
  }
  .txs li .muted {
    font-size: 0.85rem;
  }
  .link {
    background: none;
    border: none;
    padding: 0;
    color: var(--accent);
    cursor: pointer;
    text-align: start;
    word-break: break-all;
  }
</style>
