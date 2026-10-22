<script lang="ts">
  // Send: one payment on this chain, from the wallet's available coins, with the fee the user sets (these chains take a
  // fee in satoshis, not a rate). Asked in the app's trusted dialog (create_transfer, rpc.confirmed), never sent from here.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import { CHAIN } from '../config';
  import { DROPIN } from '../dropin.config';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, PAGES } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import AddressField from './AddressField.svelte';
  import SetupWallet from './SetupWallet.svelte';
  import { confirmed, sats, satsNumber } from './api';
  import { drop, follow, refresh } from './store.svelte';

  $effect(() => follow());

  let to = $state('');
  let toValid = $state(false);
  let amount = $state<bigint | null>(null);
  let fee = $state<bigint | null>(BigInt(DROPIN.defaultFee));
  let busy = $state(false);
  let error = $state('');
  let formKey = $state(0);
  let last: { txid: string; amount: bigint; to: string } | null = $state(null);

  const available = $derived(sats(drop.overview?.wallet.available));
  const total = $derived((amount ?? 0n) + (fee ?? 0n));
  const tooMuch = $derived(total > available);
  const ready = $derived(toValid && amount !== null && amount > 0n && fee !== null && !tooMuch && !busy);

  function useAll() {
    const f = fee ?? 0n;
    amount = available > f ? available - f : 0n;
    formKey++;
  }

  async function send() {
    if (!ready || amount === null || fee === null) return;
    busy = true;
    error = '';
    try {
      const txid = await confirmed<string>('create_transfer', [to.trim(), satsNumber(amount), satsNumber(fee)]);
      last = { txid, amount, to: to.trim() };
      toast('success', t('Sent'), t('The payment confirms with the next block of %1.', CHAIN.name));
      to = '';
      amount = null;
      formKey++;
      await refresh();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page drop-send">
  <header class="page-head"><h1>{t('Send')}</h1></header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <div class="layout">
      <section class="panel" aria-labelledby="send-title">
        <div class="panel-head"><h2 id="send-title">{t('Pay an address on %1', CHAIN.name)}</h2></div>
        <form
          class="panel-body stack"
          onsubmit={(e) => {
            e.preventDefault();
            send();
          }}
        >
          <AddressField id="send-to" label={t('Pay to')} bind:value={to} bind:valid={toValid} placeholder={DROPIN.addressExample} />
          {#key formKey}
            <AmountInput id="send-amount" label={t('Amount')} bind:value={amount} required max={available} />
          {/key}
          <div class="row avail">
            <span class="muted">{t('Available')}: <Amount sats={available} /></span>
            <button type="button" class="btn ghost small" onclick={useAll} disabled={available === 0n}>{t('Use all')}</button>
          </div>
          <AmountInput id="send-fee" label={t('Fee')} bind:value={fee} required title={t('Paid to whoever makes the block that takes this payment.')} />
          <div class="summary">
            <span>{t('Total')}</span>
            <strong><Amount sats={total} /></strong>
          </div>
          {#if tooMuch}<p class="warn"><Icon name="alert" size={14} /> {t('More than the wallet has available.')}</p>{/if}
          {#if error}<p class="error" role="alert"><Icon name="alert" size={14} /> {error}</p>{/if}
          <div class="row">
            <button type="submit" class="btn primary" disabled={!ready}><Icon name="send" size={16} /> {t('Review and send…')}</button>
            {#if PAGES['batchpay' as keyof typeof PAGES]}<button type="button" class="btn" onclick={() => go('batchpay' as never)}><Icon name="tabs" size={16} /> {t('Pay many at once')}</button>{/if}
          </div>
          <p class="hint">{t('You check the payment in the app’s own window before it goes. A payment cannot be taken back.')}</p>
        </form>
      </section>
      <aside class="panel side">
        <div class="panel-body">
          {#if last}
            <h3><Icon name="check" size={16} /> {t('Sent')}</h3>
            <p><Amount sats={last.amount} /> {t('to')} <span class="mono break">{last.to}</span></p>
            <p class="mono faint break">{last.txid}</p>
            <button type="button" class="btn small" onclick={() => go('transactions')}>{t('See the activity')}</button>
          {:else}
            <h3>{t('How payments work here')}</h3>
            <p class="muted">{t('Addresses on %1 are short base58 texts. A payment is confirmed when the next block of %1 is merge-mined on eCash, about every ten minutes.', CHAIN.name)}</p>
            <p class="muted">{t('To send ECX back to eCash, use Withdraw on the Drivechains page instead.')}</p>
          {/if}
        </div>
      </aside>
    </div>
  {/if}
</div>

<style>
  .drop-send {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .layout {
    display: grid;
    grid-template-columns: minmax(20rem, 34rem) minmax(0, 1fr);
    gap: 1rem;
    align-items: start;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.85rem;
  }
  .avail {
    justify-content: space-between;
    margin-top: -0.4rem;
    font-size: 0.88rem;
  }
  .summary {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    padding: 0.6rem 0.8rem;
    border-radius: 0.6rem;
    background: color-mix(in srgb, var(--accent) 7%, transparent);
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.85rem;
    margin: 0;
  }
  .warn,
  .error {
    margin: 0;
    display: flex;
    gap: 0.35rem;
    align-items: center;
  }
  .warn {
    color: var(--warning);
  }
  .error {
    color: var(--negative);
  }
  .side .panel-body {
    padding-top: 1.1rem;
  }
  .side h3 {
    margin: 0 0 0.5rem;
    font-size: 1rem;
    display: flex;
    gap: 0.4rem;
    align-items: center;
  }
  .break {
    word-break: break-all;
  }
  @media (max-width: 980px) {
    .layout {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
