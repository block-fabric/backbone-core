<script lang="ts">
  // The wallet's overview: what it holds (available, coming in, being withdrawn to eCash), and its latest activity.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import ActivityList from './ActivityList.svelte';
  import SetupWallet from './SetupWallet.svelte';
  import { sats } from './api';
  import { drop, follow, wantHistory } from './store.svelte';

  $effect(() => follow());
  $effect(() => wantHistory(20));
  const w = $derived(drop.overview?.wallet);
</script>

<div class="page drop-overview">
  <header class="page-head"><h1>{t('Overview')}</h1></header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet />
  {:else}
    <div class="balances">
      <section class="panel bal main">
        <span class="label">{t('Available')}</span>
        <span class="value"><Amount sats={sats(w?.available)} /></span>
        <div class="row">
          <button type="button" class="btn primary small" onclick={() => go('send')}><Icon name="send" size={15} /> {t('Send')}</button>
          <button type="button" class="btn small" onclick={() => go('receive')}><Icon name="receive" size={15} /> {t('Receive')}</button>
        </div>
      </section>
      <section class="panel bal">
        <span class="label">{t('Coming in')}</span>
        <span class="value"><Amount sats={sats(w?.pendingIn)} /></span>
        <span class="hint">{t('In payments waiting for the next block.')}</span>
      </section>
      <section class="panel bal">
        <span class="label">{t('Being withdrawn')}</span>
        <span class="value"><Amount sats={sats(w?.withdrawing)} /></span>
        <span class="hint">{t('On its way to eCash, in a withdrawal bundle.')}</span>
      </section>
    </div>
    <section class="panel">
      <div class="panel-head row">
        <h2 class="grow">{t('Latest activity')}</h2>
        <button type="button" class="btn ghost small" onclick={() => go('transactions')}>{t('All activity')}</button>
      </div>
      <ActivityList rows={drop.history.slice(0, 20)} empty={drop.historyLoaded ? t('Nothing yet') : t('Reading…')} />
    </section>
  {/if}
</div>

<style>
  .drop-overview {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .balances {
    display: grid;
    grid-template-columns: 1.4fr 1fr 1fr;
    gap: 1rem;
  }
  .bal {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    padding: 1rem 1.1rem;
  }
  .bal .label {
    color: var(--text-muted);
    font-size: 0.85rem;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .bal .value {
    font-size: 1.4rem;
    font-weight: 650;
  }
  .bal.main .value {
    font-size: 1.9rem;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.83rem;
  }
  @media (max-width: 900px) {
    .balances {
      grid-template-columns: 1fr;
    }
  }
</style>
