<script lang="ts">
  // What an output holds, for the explorer and the transaction dialog: ECX as an amount; on a chain of assets
  // (BitAssets), an asset output as its whole amount and its asset (its name when the wallet knows it, else its id's
  // start), a control coin, LP tokens or an auction receipt in words. Asset names are text anyone chose: shown as data.
  import Amount from '../../components/Amount.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { sats, type OutputView } from './api';
  import { groupUnits } from './kinds';

  let { o }: { o: OutputView } = $props();
  const assetText = $derived(o.assetName ?? (o.asset ? o.asset.slice(0, 10) + '…' : ''));
</script>

{#if o.kind === 'asset' && o.units !== undefined}<span class="units num">{groupUnits(o.units)} <SafeText class="asset" text={assetText} /></span>
{:else if o.kind === 'lp' && o.units !== undefined}<span class="units num">{groupUnits(o.units)} <span class="asset">{t('LP tokens')}</span></span>
{:else if o.kind === 'control'}<span class="units">{t('control coin of')} <SafeText class="asset" text={assetText} /></span>
{:else if o.kind === 'receipt'}<span class="units">{t('auction receipt')}</span>
{:else}<Amount sats={sats(o.amount)} />{/if}

<style>
  .units {
    white-space: nowrap;
  }
  .units :global(.asset) {
    color: var(--text-muted);
  }
</style>
