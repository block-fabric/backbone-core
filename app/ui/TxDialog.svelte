<script lang="ts">
  // One transaction of the wallet's activity, as the node knows it (dropin.transaction): its block, its outputs (the
  // wallet's own marked), its fee; a deposit or a bundle row says what it is about.
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Dialog from '../../components/Dialog.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatCount, formatTime } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { goTo } from '../../lib/nav.svelte';
  import { dropin, sats, type HistoryRow, type TxDetail } from './api';
  import { kindText } from './kinds';
  import { rowFacts } from './spend';

  let { row = $bindable(null) }: { row?: HistoryRow | null } = $props();
  let open = $state(false);
  let detail = $state<TxDetail | null>(null);
  let error = $state('');

  $effect(() => {
    open = row !== null;
    detail = null;
    error = '';
    const txid = row?.txid;
    if (txid) {
      dropin
        .transaction(txid)
        .then((d) => (detail = d))
        .catch((e) => (error = errorText(e)));
    }
  });
</script>

<Dialog bind:open title={row ? kindText(row) : ''} wide onclose={() => (row = null)}>
  {#if row}
    <dl class="facts">
      <dt>{t('Amount')}</dt><dd><Amount sats={sats(row.amount)} plus colored /></dd>
      {#if row.withdrawing}<dt>{t('Withdrawn to eCash')}</dt><dd><Amount sats={sats(row.withdrawing)} /></dd>{/if}
      {#if row.mainAddress}<dt>{t('eCash address')}</dt><dd class="mono break">{row.mainAddress}</dd>{/if}
      {#if row.names?.length}<dt>{t('Names')}</dt><dd class="break"><SafeText text={row.names.join(', ')} /></dd>{/if}
      {#if row.reservedName}<dt>{t('Reserved name')}</dt><dd class="break"><SafeText text={row.reservedName} /></dd>{/if}
      {#each rowFacts(row) as f (f.label)}<dt>{t(f.label)}</dt><dd class:mono={f.mono} class="break"><SafeText text={f.value} /></dd>{/each}
      {#if row.fee !== undefined}<dt>{t('Fee')}</dt><dd><Amount sats={sats(row.fee)} /></dd>{/if}
      <dt>{t('Status')}</dt><dd>{row.pending ? t('Waiting for a block') : detail?.confirmations ? t('%1 confirmations', formatCount(detail.confirmations)) : row.bundleStatus ?? t('In a block')}</dd>
      {#if row.height !== null}<dt>{t('Block')}</dt><dd class="num">{formatCount(row.height)}{#if row.approxTime} <span class="faint">({t('first seen')})</span>{/if}</dd>{/if}
      {#if row.time}<dt>{t('Date')}</dt><dd>{formatTime(row.time)}</dd>{/if}
      {#if row.txid}<dt>{t('Transaction')}</dt><dd class="mono break">{row.txid} <CopyButton text={row.txid} label={t('Copy')} /></dd>{/if}
      {#if row.m6id}<dt>{t('Bundle (M6 id)')}</dt><dd class="mono break">{row.m6id}</dd>{/if}
      {#if row.kind === 'deposit'}<dt>{t('eCash transaction')}</dt><dd class="mono break">{row.id.replace(/^deposit:/, '')}</dd>{/if}
    </dl>
    {#if detail}
      {#if detail.outputs.length}<h3>{t('Outputs')}</h3>{/if}
      <ul class="outs">
        {#each detail.outputs as o, i (i)}
          <li>
            <span class="mono break">{o.address}</span>
            {#if o.mine}<span class="badge">{t('yours')}</span>{/if}
            {#if o.kind === 'withdrawal'}<span class="badge warning">{t('withdrawal')}</span>{/if}
            <Amount sats={sats(o.amount)} />
          </li>
        {/each}
      </ul>
      {#if detail.block}<button type="button" class="btn small" onclick={() => { goTo('explorer', 'block', detail?.block ?? ''); row = null; }}>{t('Open its block')}</button>{/if}
    {:else if error}
      <p class="faint">{error}</p>
    {/if}
  {/if}
</Dialog>

<style>
  .facts {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.35rem 1rem;
    margin: 0 0 1rem;
  }
  .facts dt {
    color: var(--text-muted);
  }
  .facts dd {
    margin: 0;
  }
  h3 {
    font-size: 0.95rem;
    margin: 0.5rem 0;
  }
  .outs {
    list-style: none;
    padding: 0;
    margin: 0 0 0.8rem;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .outs li {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    justify-content: space-between;
  }
  .break {
    word-break: break-all;
  }
</style>
