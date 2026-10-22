<script lang="ts">
  // A short list of the wallet's activity (Home, Overview): its kind, the other side, when, the amount.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import { niceTimeOffset, shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { goTo } from '../../lib/nav.svelte';
  import { sats, type HistoryRow } from './api';
  import { counterparty, kindIcon, kindText } from './kinds';

  let { rows, now = Math.floor(Date.now() / 1000), empty }: { rows: HistoryRow[]; now?: number; empty: string } = $props();
</script>

{#if rows.length === 0}
  <p class="muted empty">{empty}</p>
{:else}
  <ul class="activity">
    {#each rows as row (row.id)}
      <li>
        <button type="button" class="line" onclick={() => goTo('transactions', '', row.id)}>
          <span class="ico {row.amount > 0 ? 'in' : row.amount < 0 ? 'out' : ''}"><Icon name={kindIcon(row)} size={16} /></span>
          <span class="what">
            <strong>{kindText(row)}</strong>
            <span class="mono faint">{counterparty(row) ? shortHash(counterparty(row), 10) : row.txid ? shortHash(row.txid, 8) : ''}</span>
          </span>
          <span class="when muted">
            {#if row.pending}<span class="badge warning">{t('pending')}</span>
            {:else if row.time}{t('%1 ago', niceTimeOffset(Math.max(0, now - row.time)))}{/if}
          </span>
          <span class="amt">{#if row.kind === 'bundle'}<span class="faint"><Amount sats={sats(row.withdrawing)} /></span>{:else}<Amount sats={sats(row.amount)} plus colored />{/if}</span>
        </button>
      </li>
    {/each}
  </ul>
{/if}

<style>
  .activity {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .line {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto auto;
    gap: 0.75rem;
    align-items: center;
    width: 100%;
    padding: 0.55rem 1rem;
    background: none;
    border: none;
    border-top: 1px solid var(--border);
    color: var(--text);
    text-align: start;
    cursor: pointer;
  }
  li:first-child .line {
    border-top: none;
  }
  .line:hover {
    background: var(--hover, color-mix(in srgb, var(--accent) 6%, transparent));
  }
  .ico {
    display: grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    border-radius: 50%;
    background: color-mix(in srgb, var(--text-muted) 14%, transparent);
    color: var(--text-muted);
  }
  .ico.in {
    background: color-mix(in srgb, var(--positive) 16%, transparent);
    color: var(--positive);
  }
  .ico.out {
    background: color-mix(in srgb, var(--accent) 14%, transparent);
    color: var(--accent);
  }
  .what {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .what .mono {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 0.82rem;
  }
  .when {
    font-size: 0.85rem;
  }
  .empty {
    padding: 1rem;
    margin: 0;
  }
</style>
