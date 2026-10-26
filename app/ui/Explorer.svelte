<script lang="ts">
  // The block explorer of this chain, laid out as the kit's (pages/Explorer.svelte, Flush's and every app's): a search (a
  // height, a block or transaction hash, an address or a deposit address) with how many of the latest blocks are read
  // ("Last N blocks"); those blocks as the kit's ribbon of boxes, then as a table in pages (rows per page); a block, a
  // transaction or an address opened shows below the table, on the same page, with Back along what was shown. A chain's
  // own facts of a block come from its dropin.config (explorer.blockFacts). Read from this node (dropin.blocks, at most
  // DROPIN_PART blocks a call, the newest first; on a new tip only the new ones are read).
  import Amount from '../../components/Amount.svelte';
  import BlockRibbon from '../../components/BlockRibbon.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import DataTable from '../../components/DataTable.svelte';
  import Icon from '../../components/Icon.svelte';
  import Meter from '../../components/Meter.svelte';
  import Pager from '../../components/Pager.svelte';
  import { css } from '../../lib/actions';
  import { TARGET_SPACING, type RibbonBlock } from '../../lib/blocks';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatBytes, formatCount, formatTime, shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav, takeFocus } from '../../lib/nav.svelte';
  import { every } from '../../lib/poll';
  import { prefs, savePrefs } from '../../lib/state.svelte';
  import { filterAndSort, type Column } from '../../lib/table';
  import { unitInfo } from '../../lib/units';
  import { DROPIN } from '../dropin.config';
  import { dropin, sats, type AddressDetail, type BlockDetail, type BlockSummary, type TxDetail } from './api';
  import { drop, follow } from './store.svelte';
  import { untrack } from 'svelte';

  $effect(() => follow());

  /** A chain's own facts of a block, shown with the shared ones (its dropin.config's explorer.blockFacts). */
  type Fact = { dt: string; dd: string; mono?: boolean };
  const chainFacts = (DROPIN as unknown as { explorer?: { blockFacts?: (b: BlockDetail) => Fact[] } }).explorer?.blockFacts;

  // How many of the latest blocks are read, and how many a page shows: the kit's choices and preferences (0: all).
  const BLOCK_POINTS = [25, 50, 100, 250, 500, 1000, 10000, 50000, 0];
  const BLOCK_PAGE_SIZES = [10, 25, 50, 100];
  /** The most blocks a dropin.blocks call gives (the chains' hosts clamp it to 200). */
  const DROPIN_PART = 200;
  /** The height of a row of the list (DataTable's), to size the list to its page. */
  const ROW = 34;
  const blockPoints = $derived(BLOCK_POINTS.includes(prefs.explorerCount) ? BLOCK_POINTS : [...BLOCK_POINTS.filter((n) => n > 0), prefs.explorerCount].sort((a, b) => a - b).concat(0));

  type Shown = { kind: 'block'; detail: BlockDetail } | { kind: 'tx'; detail: TxDetail } | { kind: 'address'; detail: AddressDetail };
  /** What Back returns to. */
  type View = { kind: 'block'; hash: string } | { kind: 'tx'; txid: string } | { kind: 'address'; address: string };
  const MAX_TRAIL = 50;

  let blocks = $state.raw<BlockSummary[]>([]);
  let loaded = $state(false);
  let query = $state('');
  let error = $state('');
  let loading = $state(false);
  let selected = $state<string | null>(null);
  let sortKey = $state<string | null>('height');
  let sortDir = $state<'asc' | 'desc'>('desc');
  let page = $state(0);
  let shown = $state<Shown | null>(null);
  let trail = $state.raw<View[]>([]);

  // Another order: from its first page.
  $effect(() => {
    void sortKey;
    void sortDir;
    page = 0;
  });

  // --- The list: the latest prefs.explorerCount blocks, in parts; on a new tip only the new ones read ---
  /** Up to `count` blocks from the height `from` down; fewer when the chain is shorter (or moved meanwhile). */
  async function readRows(from: number, count: number): Promise<BlockSummary[]> {
    const out: BlockSummary[] = [];
    while (out.length < count && from >= 0) {
      const part = await dropin.blocks(Math.min(DROPIN_PART, count - out.length), from);
      if (!part.length || part[0].height !== from) break;
      out.push(...part);
      from = part[part.length - 1].height - 1;
    }
    return out;
  }
  async function loadBlocks() {
    const [top] = await dropin.blocks(1);
    if (!top) {
      blocks = [];
      return;
    }
    const count = prefs.explorerCount;
    const wanted = count === 0 ? top.height + 1 : Math.min(count, top.height + 1);
    if (blocks.length === wanted && blocks[0]?.hash === top.hash) return;
    // The rows kept: those below the new ones, if the first one is still in the chain (no reorganization past it).
    let kept: BlockSummary[] = [];
    const first = blocks[0];
    if (first && first.height <= top.height) {
      const [same] = await dropin.blocks(1, first.height);
      if (same && same.hash === first.hash) kept = blocks;
    }
    const fresh = await readRows(top.height, Math.min(wanted, kept.length ? top.height - kept[0].height : wanted));
    let next = [...fresh, ...kept].slice(0, wanted);
    if (next.length < wanted && next.length && next[next.length - 1].height > 0) next = next.concat(await readRows(next[next.length - 1].height - 1, wanted - next.length));
    blocks = next;
  }
  let reading: Promise<void> | null = null;
  let again = false;
  async function refreshBlocks() {
    if (reading) {
      again = true;
      return reading;
    }
    reading = (async () => {
      do {
        again = false;
        try {
          await loadBlocks();
          error = '';
        } catch (e) {
          error = errorText(e);
        }
      } while (again);
      loaded = true;
    })();
    try {
      await reading;
    } finally {
      reading = null;
    }
  }
  $effect(() => {
    void drop.version;
    void prefs.explorerCount;
    void refreshBlocks();
  });

  const columns: Column<BlockSummary>[] = $derived([
    { key: 'height', label: 'Height', value: (b) => b.height, width: '6rem', align: 'end' },
    { key: 'time', label: 'Time', value: (b) => b.time ?? 0, width: '11.5rem' },
    { key: 'txs', label: 'Transactions', value: (b) => b.txs, width: '7.5rem', align: 'end' },
    { key: 'fill', label: 'Size', value: (b) => b.bytes, width: '10rem' },
    { key: 'fees', label: `Fees (${unitInfo(prefs.unit).short})`, value: (b) => b.fees, width: '9rem', align: 'end' },
    { key: 'hash', label: 'Hash', value: (b) => b.hash, width: 'minmax(14rem, 1fr)', mono: true },
  ]);
  const sorted = $derived(filterAndSort(blocks, columns, '', sortKey, sortDir));
  const pageRows = $derived(sorted.slice(page * prefs.explorerPageSize, (page + 1) * prefs.explorerPageSize));

  // --- The ribbon: the blocks read, newest first, as the kit's boxes ---
  const RIBBON_MAX = 1000;
  let nowSecs = $state(Math.floor(Date.now() / 1000));
  $effect(() => every(15000, () => (nowSecs = Math.floor(Date.now() / 1000))));
  const ribbon = $derived(
    blocks.slice(0, RIBBON_MAX).map(
      (b): RibbonBlock => ({
        height: b.height,
        hash: b.hash,
        time: b.time ?? 0,
        nTx: b.txs,
        weight: null,
        // No subsidy on a drop-in chain: what a block's maker gets is its fees.
        subsidy: 0,
        fees: Number(sats(b.fees)),
        feeRate: null,
        tag: '',
        bmm: [],
        bytes: b.bytes,
        limit: b.limit,
      }),
    ),
  );
  // A new block while the page is shown: it slides in once.
  let fresh = $state<string | null>(null);
  let topSeen: string | null = null;
  $effect(() => {
    const top = blocks[0]?.hash ?? null;
    if (!top) return;
    const changed = topSeen !== null && topSeen !== top;
    topSeen = top;
    if (!changed) return;
    fresh = top;
    const timer = setTimeout(() => (fresh = null), 1500);
    return () => clearTimeout(timer);
  });

  // --- What is shown below the table, and Back along it ---
  function viewOf(s: Shown | null): View | null {
    if (s?.kind === 'block') return { kind: 'block', hash: s.detail.hash };
    if (s?.kind === 'tx') return { kind: 'tx', txid: s.detail.txid };
    if (s?.kind === 'address') return { kind: 'address', address: s.detail.address };
    return null;
  }
  const sameView = (a: View, b: View) => JSON.stringify(a) === JSON.stringify(b);
  async function display(v: View): Promise<void> {
    if (v.kind === 'block') {
      const detail = await dropin.block({ hash: v.hash });
      shown = { kind: 'block', detail };
      // The row of the block, selected (the list's keys go on from it).
      selected = detail.hash;
    } else {
      shown = v.kind === 'tx' ? { kind: 'tx', detail: await dropin.transaction(v.txid) } : { kind: 'address', detail: await dropin.address(v.address) };
      // No block shown: a click on any row opens its block again.
      selected = null;
    }
  }
  /** Open a view below the table; the one shown before goes on the trail Back returns along. */
  async function open(v: View): Promise<boolean> {
    const before = viewOf(shown);
    if (before && sameView(before, v)) return true;
    try {
      await display(v);
      error = '';
    } catch (e) {
      error = errorText(e);
      return false;
    }
    if (before) trail = [...trail, before].slice(-MAX_TRAIL);
    queueMicrotask(() => document.getElementById('explorer-title')?.scrollIntoView?.({ block: 'nearest' }));
    return true;
  }
  async function back() {
    const v = trail[trail.length - 1];
    if (!v) return;
    trail = trail.slice(0, -1);
    try {
      await display(v);
    } catch (e) {
      error = errorText(e);
    }
  }
  const openBlock = (hash: string) => open({ kind: 'block', hash });
  const openTx = (txid: string) => open({ kind: 'tx', txid });
  const openAddress = (address: string) => open({ kind: 'address', address });
  async function openHeight(height: number) {
    try {
      const detail = await dropin.block({ height });
      await openBlock(detail.hash);
    } catch (e) {
      error = errorText(e);
    }
  }

  // A click on a row (or the keys moving along the list) shows its block below the table.
  $effect(() => {
    const hash = selected;
    untrack(() => {
      if (hash && (shown?.kind !== 'block' || shown.detail.hash !== hash)) void openBlock(hash);
    });
  });

  async function search(input = query) {
    const q = input.trim();
    if (!q) return;
    loading = true;
    try {
      const r = await dropin.search(q);
      if (r.kind === 'block' && r.hash) await openBlock(r.hash);
      else if (r.kind === 'transaction' && r.txid) await openTx(r.txid);
      else if (r.kind === 'address' && r.address) await openAddress(r.address);
      else error = t('Nothing of this chain is called %1', q);
    } catch (e) {
      error = errorText(e);
    } finally {
      loading = false;
    }
  }

  // Deep links: a block given with the page (go('explorer', '', hash), the activity's "Open its block"), or in its tab:
  // a height or a block hash ("block:<hash or height>"), or anything to search ("search:<text>").
  $effect(() => {
    const focus = takeFocus('explorer');
    if (focus) void (/^[0-9]{1,9}$/.test(focus) ? openHeight(Number(focus)) : openBlock(focus));
  });
  $effect(() => {
    const tab = nav.tab;
    const block = /^block:([0-9]{1,9}|[0-9a-fA-F]{64})$/.exec(tab);
    const asked = tab.startsWith('search:') ? tab.slice('search:'.length) : null;
    if (!block && asked === null) return;
    nav.tab = '';
    if (block) void (/^[0-9]+$/.test(block[1]) ? openHeight(Number(block[1])) : openBlock(block[1].toLowerCase()));
    else if (asked !== null) {
      query = asked;
      void search(asked);
    }
  });
</script>

<div class="page drop-explorer">
  <header class="page-head"><h1>{t('Block explorer')}</h1></header>
  <form
    class="search row"
    onsubmit={(e) => {
      e.preventDefault();
      search();
    }}
  >
    <div class="box grow">
      <Icon name="search" size={17} />
      <input id="explorer-search" type="search" class="mono" placeholder={t('Block height, block hash, transaction id or address')} spellcheck="false" autocomplete="off" bind:value={query} aria-label={t('Search the chain')} />
    </div>
    <button type="submit" class="btn primary" disabled={loading}>{t('Search')}</button>
    <label class="row count">
      <span class="muted">{t('Last')}</span>
      <select
        value={prefs.explorerCount}
        onchange={(e) => {
          savePrefs({ explorerCount: Number((e.currentTarget as HTMLSelectElement).value) });
          page = 0;
        }}
        aria-label={t('Blocks listed')}
      >
        {#each blockPoints as n (n)}<option value={n}>{n === 0 ? t('All') : formatCount(n)}</option>{/each}
      </select>
      <span class="muted">{t('blocks')}</span>
    </label>
    <button type="button" class="btn" onclick={() => ((blocks = []), refreshBlocks())}>{t('Refresh')}</button>
  </form>
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}

  {#if ribbon.length}
    <BlockRibbon blocks={ribbon} {selected} {fresh} now={nowSecs} onselect={(hash) => (selected = hash)} blockTarget={TARGET_SPACING} />
  {/if}

  <div class="list-bar">
    <h2 class="caps">{t('Blocks')}</h2>
    <div class="block-pages"><Pager total={sorted.length} bind:page size={prefs.explorerPageSize} sizes={BLOCK_PAGE_SIZES} label="blocks" onsize={(n) => savePrefs({ explorerPageSize: n })} /></div>
  </div>
  <!-- As tall as the rows of the page: the page scrolls, not the list. -->
  <div class="table-box" use:css={{ height: `${(Math.max(1, pageRows.length) + 1) * ROW + 2}px` }}>
    <DataTable fit rows={pageRows} {columns} key={(b) => b.hash} bind:selected bind:sortKey bind:sortDir label={t('Blocks')} empty={loaded ? t('No block yet') : t('Loading…')} onactivate={(b) => openBlock(b.hash)}>
      {#snippet cell(row, column)}
        {#if column.key === 'height'}<button type="button" class="mono linkish" onclick={() => (selected = row.hash)}>{formatCount(row.height)}</button>
        {:else if column.key === 'time'}{row.time ? formatTime(row.time) : '–'}
        {:else if column.key === 'hash'}<button type="button" class="mono linkish cell-link" onclick={() => (selected = row.hash)}>{row.hash}</button>
        {:else if column.key === 'txs'}<span class="num">{formatCount(row.txs)}</span>
        {:else if column.key === 'fill'}{#if row.limit > 0}<Meter value={row.bytes} max={row.limit} label={t('How full')} text={formatBytes(row.bytes)} tone={row.bytes > row.limit * 0.8 ? 'warning' : 'accent'} />{:else}<span class="num">{formatBytes(row.bytes)}</span>{/if}
        {:else}<Amount sats={sats(row.fees)} showUnit={false} />{/if}
      {/snippet}
    </DataTable>
  </div>
  <p class="faint hint">{t('Click a block to see its details below, or search for one.')}</p>

  {#snippet backButton()}
    {#if trail.length}<button type="button" class="btn small ghost" onclick={back} title={t('Back to what was shown before')}><Icon name="receive" size={14} /> {t('Back')}</button>{/if}
  {/snippet}

  {#if shown?.kind === 'block'}
    {@const b = shown.detail}
    <section class="panel detail" aria-label={t('Block %1', formatCount(b.height))}>
      <div class="panel-head">
        <h2 id="explorer-title">{t('Block %1', formatCount(b.height))} {#if !b.onChain}<span class="badge warning">{t('not on the chain')}</span>{/if}</h2>
        <div class="row">
          {@render backButton()}
          {#if b.prevHash}<button type="button" class="btn small" onclick={() => openBlock(b.prevHash)}><Icon name="receive" size={14} /> {t('Previous')}</button>{/if}
          {#if b.onChain && b.height < (blocks[0]?.height ?? -1)}<button type="button" class="btn small" onclick={() => openHeight(b.height + 1)}>{t('Next')} <Icon name="send" size={14} /></button>{/if}
        </div>
      </div>
      <div class="panel-body block-grid">
        <dl class="facts">
          <dt>{t('Hash')}</dt><dd class="mono break">{b.hash} <CopyButton text={b.hash} label={t('Copy the block hash')} /></dd>
          {#if b.time}<dt>{t('Time')}</dt><dd>{formatTime(b.time)}</dd>{/if}
          <dt>{t('Transactions')}</dt><dd class="num">{formatCount(b.txs)}</dd>
          <dt>{t('Size')}</dt><dd>{formatBytes(b.bytes)}{#if b.limit > 0} / {formatBytes(b.limit)}{/if}</dd>
          <dt>{t('Fees')}</dt><dd><Amount sats={sats(b.fees)} /></dd>
          {#if b.prevHash}<dt>{t('Previous block')}</dt><dd class="mono break"><button type="button" class="mono linkish" onclick={() => openBlock(b.prevHash)}><span class="faint">{formatCount(b.height - 1)} ·</span> {b.prevHash}</button></dd>{:else}<dt>{t('Previous block')}</dt><dd>{t('none (the first block)')}</dd>{/if}
          <dt>{t('Merged mined in')}</dt><dd class="mono break main-ref"><span class="faint">eCash ·</span> {b.mainHash} <CopyButton text={b.mainHash} label={t('Copy the hash of the block of %1', 'eCash')} /></dd>
          {#if b.merkleRoot}<dt>{t('Merkle root')}</dt><dd class="mono break">{b.merkleRoot}</dd>{/if}
          {#each chainFacts?.(b) ?? [] as f (f.dt)}<dt>{f.dt}</dt><dd class:mono={f.mono} class="break">{f.dd}</dd>{/each}
        </dl>
        <div>
          {#if b.coinbase.length}
            <h3>{t('Paid to the block’s maker')}</h3>
            <ul class="outs">{#each b.coinbase as o, i (i)}<li><button type="button" class="mono linkish" onclick={() => openAddress(o.address)}>{o.address}</button><Amount sats={sats(o.amount)} /></li>{/each}</ul>
          {/if}
          <h3>{t('Transactions of the block')}</h3>
          {#if b.transactions.length === 0}<p class="muted">{t('None')}</p>{/if}
          <ul class="txs" aria-label={t('Transactions of the block')}>
            {#each b.transactions as tx, i (tx.txid)}
              <li>
                <button type="button" class="mono" onclick={() => openTx(tx.txid)}><span class="faint">{i}</span> {tx.txid}</button>
                <span class="muted small">{t('%1 inputs, %2 outputs', tx.inputs, tx.outputs.length)}</span>
                <Amount sats={sats(tx.outputs.reduce((s, o) => s + o.amount, 0))} />
              </li>
            {/each}
          </ul>
        </div>
      </div>
    </section>
  {:else if shown?.kind === 'tx'}
    {@const tx = shown.detail}
    <section class="panel detail" aria-label={t('Transaction')}>
      <div class="panel-head">
        <h2 id="explorer-title">{t('Transaction')} {#if tx.pending}<span class="badge warning">{t('waiting for a block')}</span>{/if}</h2>
        <div class="row">{@render backButton()}</div>
      </div>
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
  {:else if shown?.kind === 'address'}
    {@const a = shown.detail}
    <section class="panel detail" aria-label={t('Address')}>
      <div class="panel-head">
        <h2 id="explorer-title">{t('Address')} {#if a.mine}<span class="badge">{t('yours')}</span>{/if}</h2>
        <div class="row">{@render backButton()}</div>
      </div>
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
  /* The kit's explorer (pages/Explorer.svelte): the same search, list bar, table and detail below. */
  .search {
    margin-top: 0.6rem;
    flex-wrap: wrap;
  }
  .box {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0 0.6rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-s);
    background: var(--surface);
    color: var(--text-muted);
  }
  .box input {
    border: 0;
    padding-left: 0;
    background: transparent;
  }
  .box input:focus-visible {
    outline: none;
  }
  .box:has(input:focus-visible) {
    outline: 2px solid var(--focus);
    outline-offset: 2px;
  }
  .count select {
    width: auto;
  }
  .list-bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem 1rem;
  }
  .list-bar h2 {
    margin: 0;
    font-size: 0.8rem;
  }
  .block-pages {
    margin-left: auto;
  }
  .block-pages :global(.pager) {
    padding: 0;
  }
  .table-box {
    min-height: 0;
  }
  .hint {
    margin: 0.4rem 0 1rem;
    font-size: 0.86rem;
  }
  .error {
    color: var(--negative);
    margin: 0.4rem 0 0;
    display: flex;
    gap: 0.35rem;
    align-items: center;
  }
  .detail {
    margin-bottom: 1rem;
  }
  .block-grid {
    display: grid;
    grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr);
    gap: 1.2rem;
  }
  @media (max-width: 900px) {
    .block-grid {
      grid-template-columns: 1fr;
    }
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
    margin: 0 0 0.6rem;
  }
  .outs,
  .txs {
    list-style: none;
    padding: 0;
    margin: 0 0 1rem;
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
  .txs button,
  .linkish,
  .link {
    background: none;
    border: none;
    padding: 0;
    color: var(--accent);
    cursor: pointer;
    text-align: start;
    word-break: break-all;
  }
  .linkish:hover,
  .linkish:focus-visible,
  .txs button:hover {
    text-decoration: underline;
  }
  .cell-link {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 100%;
    word-break: normal;
  }
  .main-ref {
    color: var(--text-muted);
  }
  .small {
    font-size: 0.85rem;
  }
</style>
