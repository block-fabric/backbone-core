<script lang="ts">
  // The home of a drop-in chain: the cards of the kit's dashboard (views/home/Card.svelte) over this node: the balance,
  // the chain and its link to eCash, the peg; the chain's own card (its LocalCards); the latest activity. Until the
  // wallet has a backup phrase, setting it up comes first.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import Card from '../../views/home/Card.svelte';
  import LocalCards from '../LocalCards.svelte';
  import { CHAIN } from '../config';
  import { formatCount, niceTimeOffset } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { mainLink } from '../../lib/mainlink.svelte';
  import { sats } from './api';
  import { BALANCE_TITLE } from './spend';
  import ActivityList from './ActivityList.svelte';
  import SetupWallet from './SetupWallet.svelte';
  import { drop, follow, wantHistory } from './store.svelte';
  import { linkState, linkText } from './link';

  $effect(() => follow());
  $effect(() => wantHistory(8));

  const o = $derived(drop.overview);
  const w = $derived(o?.wallet);
  const hasSeed = $derived(!!drop.seed?.hasSeed);
  const link = $derived(o?.mainchain ?? mainLink.info);
  const ls = $derived(linkState(link));
  const recent = $derived(drop.history.slice(0, 8));
  const now = $derived(Math.floor(Date.now() / 1000) + drop.version * 0);
</script>

<div class="page drop-home">
  <header class="page-head">
    <h1>{CHAIN.name}</h1>
    {#if o}<span class="sub muted">{t('Slot %1 on eCash', o.chain.slot)}{o.chain.network && o.chain.network !== 'betanet' ? ` · ${o.chain.network}` : ''}</span>{/if}
  </header>

  {#if drop.error && !o}
    <p class="panel error-box" role="alert"><Icon name="alert" size={16} /> {drop.error}</p>
  {/if}

  {#if drop.seed && !hasSeed}
    <SetupWallet />
  {/if}

  <div class="cards">
    <Card title={t(BALANCE_TITLE)} icon="wallet" open={() => go('overview')} openLabel={t('Open the overview')} stat>
      {#if w && hasSeed}
        <div class="big"><Amount sats={sats(w.available)} /></div>
        <dl class="facts">
          {#if w.pendingIn}<dt>{t('Incoming')}</dt><dd><Amount sats={sats(w.pendingIn)} /></dd>{/if}
          {#if w.withdrawing}<dt>{t('Being withdrawn')}</dt><dd><Amount sats={sats(w.withdrawing)} /></dd>{/if}
          {#if w.pendingOut}<dt>{t('Leaving')}</dt><dd><Amount sats={sats(w.pendingOut)} /></dd>{/if}
        </dl>
      {:else}
        <p class="muted">{hasSeed ? t('Reading the wallet…') : t('No wallet yet')}</p>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn primary small" disabled={!hasSeed} onclick={() => go('send')}><Icon name="send" size={15} /> {t('Send')}</button>
        <button type="button" class="btn small" disabled={!hasSeed} onclick={() => go('receive')}><Icon name="receive" size={15} /> {t('Receive')}</button>
      {/snippet}
    </Card>

    <Card title={t('Network')} icon="block" open={() => go('node')} openLabel={t('Open the node')}>
      {#if o}
        <div class="big num">{o.chain.height >= 0 ? formatCount(o.chain.height) : '–'}<span class="unit">{t('blocks')}</span></div>
        <dl class="facts">
          <dt>{t('Peers')}</dt><dd class="num">{o.chain.peers}</dd>
          <dt>eCash</dt><dd><span class="dot {ls.tone}"></span> {linkText(link)}</dd>
          {#if link && link.height >= 0}<dt>{t('eCash block')}</dt><dd class="num">{formatCount(link.height)}</dd>{/if}
        </dl>
      {:else}
        <p class="muted">{t('Reading the node…')}</p>
      {/if}
    </Card>

    <Card title={t('Drivechains')} icon="peg" open={() => go('peg')} openLabel={t('Open Drivechains')}>
      {#if o}
        <dl class="facts">
          <dt>{t('Held for %1', CHAIN.name)}</dt><dd><Amount sats={sats(o.chain.wealth)} /></dd>
          {#if w?.withdrawing}<dt>{t('Your withdrawals')}</dt><dd><Amount sats={sats(w.withdrawing)} /></dd>{/if}
        </dl>
        <p class="muted small">{t('Bring ECX in from eCash with a deposit, send it back with a withdrawal.')}</p>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn small" onclick={() => go('peg', 'deposit')}><Icon name="down" size={15} /> {t('Deposit')}</button>
        <button type="button" class="btn small" disabled={!hasSeed} onclick={() => go('peg', 'withdraw')}><Icon name="up" size={15} /> {t('Withdraw')}</button>
      {/snippet}
    </Card>

    <LocalCards />
  </div>

  {#if hasSeed}
    <section class="panel recent" aria-labelledby="recent-title">
      <div class="panel-head row">
        <h2 id="recent-title" class="grow">{t('Latest activity')}</h2>
        <button type="button" class="btn ghost small" onclick={() => go('transactions')}>{t('All activity')} <Icon name="chevron" size={14} /></button>
      </div>
      <ActivityList rows={recent} {now} empty={drop.historyLoaded ? t('Nothing yet: receive ECX, or deposit from eCash.') : t('Reading…')} />
    </section>
  {/if}
</div>

<style>
  .drop-home {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .page-head {
    display: flex;
    align-items: baseline;
    gap: 0.75rem;
  }
  .sub {
    font-size: 0.9rem;
  }
  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(17rem, 1fr));
    gap: 1rem;
  }
  .big {
    font-size: 1.65rem;
    font-weight: 650;
    letter-spacing: -0.01em;
    margin-bottom: 0.4rem;
  }
  .big .unit {
    font-size: 0.85rem;
    font-weight: 500;
    color: var(--text-muted);
    margin-inline-start: 0.4rem;
  }
  .facts {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 0.25rem 0.9rem;
    margin: 0;
    font-size: 0.9rem;
  }
  .facts dt {
    color: var(--text-muted);
  }
  .facts dd {
    margin: 0;
    text-align: end;
  }
  .small {
    font-size: 0.85rem;
    margin: 0.5rem 0 0;
  }
  .dot {
    display: inline-block;
    width: 0.55rem;
    height: 0.55rem;
    border-radius: 50%;
    background: var(--text-faint, var(--text-muted));
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
  .error-box {
    color: var(--negative);
    padding: 0.8rem 1rem;
    display: flex;
    gap: 0.5rem;
    align-items: center;
  }
</style>
