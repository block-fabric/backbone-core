<script lang="ts">
  // The wallet's activity (the kit's Transactions place): every transaction touching the wallet, newest first, from the
  // node's state (what it holds, what it spent, what waits in the mempool); deposits, withdrawals and their bundles in
  // words. Search, sort, a row's details, and a CSV export to a file the user chooses.
  import Amount from '../../components/Amount.svelte';
  import DataTable from '../../components/DataTable.svelte';
  import Icon from '../../components/Icon.svelte';
  import { CHAIN } from '../config';
  import { errorText } from '../../lib/dialogs.svelte';
  import { saveTextAs } from '../../lib/files';
  import { formatTime } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { takeFocus } from '../../lib/nav.svelte';
  import { prefs, toast } from '../../lib/state.svelte';
  import type { Column } from '../../lib/table';
  import { formatAmount, unitInfo } from '../../lib/units';
  import SetupWallet from './SetupWallet.svelte';
  import TxDialog from './TxDialog.svelte';
  import { sats, type HistoryRow } from './api';
  import { counterparty, kindText } from './kinds';
  import { drop, follow, wantHistory } from './store.svelte';

  $effect(() => follow());
  $effect(() => wantHistory(2000));

  let filter = $state('');
  let kind = $state('');
  let selected = $state<string | null>(takeFocus('transactions'));
  let sortKey = $state<string | null>('date');
  let sortDir = $state<'asc' | 'desc'>('desc');
  let shown = $state<HistoryRow | null>(null);

  $effect(() => {
    // Opened for a row (Home's activity): its details at once.
    if (selected && !shown && drop.historyLoaded) {
      const r = drop.history.find((x) => x.id === selected);
      if (r) shown = r;
    }
  });

  const rows = $derived(kind ? drop.history.filter((r) => r.kind === kind) : drop.history);
  const columns: Column<HistoryRow>[] = $derived([
    { key: 'date', label: 'Date', value: (r) => (r.pending ? Number.MAX_SAFE_INTEGER : (r.time ?? 0)), width: '11rem' },
    { key: 'kind', label: 'Type', value: (r) => kindText(r), width: '12rem', words: true },
    { key: 'other', label: 'Address', value: (r) => counterparty(r), width: 'minmax(8rem, 1fr)', mono: true },
    { key: 'amount', label: `Amount (${unitInfo(prefs.unit).short})`, value: (r) => r.amount, width: '12rem', align: 'end' },
  ]);

  const KINDS = [
    { id: '', label: 'All' },
    { id: 'receive', label: 'Received' },
    { id: 'send', label: 'Sent' },
    { id: 'deposit', label: 'Deposits' },
    { id: 'withdrawal', label: 'Withdrawals' },
    { id: 'bundle', label: 'Bundles' },
    { id: 'mined', label: 'Block fees' },
  ];

  async function exportCsv() {
    const esc = (s: string) => (/[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s);
    const lines = [['Date', 'Type', 'Address', `Amount (${CHAIN.coin.ticker})`, 'Fee', 'Transaction', 'Block', 'Status'].join(',')];
    for (const r of drop.history) {
      lines.push(
        [r.time ? new Date(r.time * 1000).toISOString() : '', kindText(r), counterparty(r), formatAmount(sats(r.amount), 'CHN', { plus: true }), r.fee !== undefined ? formatAmount(sats(r.fee), 'CHN') : '', r.txid ?? r.m6id ?? r.id, r.height ?? '', r.pending ? 'pending' : (r.bundleStatus ?? 'confirmed')]
          .map((v) => esc(String(v)))
          .join(','),
      );
    }
    try {
      const path = await saveTextAs({ title: t('Export the activity'), name: `${CHAIN.bin}-activity.csv`, filters: [{ name: 'CSV', patterns: ['*.csv'] }] }, lines.join('\n') + '\n');
      if (path) toast('success', t('Exported'), path);
    } catch (e) {
      toast('error', t('Export failed'), errorText(e));
    }
  }
</script>

<div class="page drop-history">
  <header class="page-head"><h1>{t('Activity')}</h1></header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <div class="row head">
      <select aria-label={t('Type')} bind:value={kind}>
        {#each KINDS as k (k.id)}<option value={k.id}>{t(k.label)}</option>{/each}
      </select>
      <input type="search" class="grow" placeholder={t('Search')} aria-label={t('Search the activity')} bind:value={filter} />
      <button type="button" class="btn small" onclick={exportCsv} disabled={drop.history.length === 0}><Icon name="download" size={15} /> {t('Export…')}</button>
    </div>
    <div class="holder">
      <DataTable
        {rows}
        {columns}
        key={(r) => r.id}
        {filter}
        bind:selected
        bind:sortKey
        bind:sortDir
        label={t('Activity')}
        empty={drop.historyLoaded ? t('Nothing yet') : t('Reading…')}
        onactivate={(r) => (shown = r)}
      >
        {#snippet cell(row, column)}
          {#if column.key === 'date'}{#if row.pending}<span class="badge warning">{t('pending')}</span>{:else if row.time}{formatTime(row.time)}{:else}<span class="faint">–</span>{/if}
          {:else if column.key === 'kind'}{kindText(row)}
          {:else if column.key === 'other'}<span class="mono clip" title={counterparty(row)}>{counterparty(row)}</span>
          {:else if row.kind === 'bundle'}<span class="faint"><Amount sats={sats(row.withdrawing)} showUnit={false} /></span>
          {:else}<Amount sats={sats(row.amount)} plus colored showUnit={false} />{/if}
        {/snippet}
      </DataTable>
    </div>
  {/if}
</div>

<TxDialog bind:row={shown} />

<style>
  .drop-history {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
    height: 100%;
  }
  .head {
    gap: 0.5rem;
    flex-wrap: nowrap;
  }
  .head select {
    width: 12rem;
    flex: none;
  }
  .clip {
    display: block;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .holder {
    flex: 1;
    min-height: 18rem;
  }
</style>
