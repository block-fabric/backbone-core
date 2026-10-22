<script lang="ts">
  // An address typed or pasted: checked by the node as it is typed (dropin.checkAddress), never guessed here. A Thunder
  // address on a Send form, an eCash one on a withdrawal; the wrong kind is said in words (an eCash address on Send, a
  // deposit address, an address of another eCash network).
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { host } from '../../lib/rpc';
  import { dropin } from './api';

  let {
    value = $bindable(''),
    valid = $bindable(false),
    id,
    label,
    side = 'chain',
    placeholder = '',
    disabled = false,
  }: { value?: string; valid?: boolean; id: string; label: string; side?: 'chain' | 'mainchain'; placeholder?: string; disabled?: boolean } = $props();

  let error = $state('');
  let mine = $state(false);
  let checking = $state(false);
  let seq = 0;

  $effect(() => {
    const text = value.trim();
    const mySeq = ++seq;
    valid = false;
    mine = false;
    error = '';
    if (!text) return;
    checking = true;
    const timer = setTimeout(async () => {
      try {
        const r = await dropin.checkAddress(text, side);
        if (mySeq !== seq) return;
        valid = r.valid;
        mine = !!r.mine;
        error = r.valid ? '' : (r.error ?? 'not an address');
      } catch (e) {
        if (mySeq === seq) error = e instanceof Error ? e.message : String(e);
      } finally {
        if (mySeq === seq) checking = false;
      }
    }, 180);
    return () => clearTimeout(timer);
  });

  async function paste() {
    try {
      const text = await host.paste();
      if (text) value = text.trim();
    } catch {
      // Not right after a gesture: the clipboard stays closed.
    }
  }
</script>

<div class="field addr">
  <label for={id}>{label}</label>
  <div class="row">
    <input {id} type="text" class="mono grow" spellcheck="false" autocomplete="off" {placeholder} {disabled} bind:value aria-invalid={!!error} aria-describedby="{id}-state" />
    <button type="button" class="btn ghost icon" title={t('Paste')} aria-label={t('Paste')} {disabled} onclick={paste}><Icon name="paste" size={16} /></button>
  </div>
  <span id="{id}-state" class="state" class:bad={!!error} class:good={valid}>
    {#if !value.trim()}&nbsp;
    {:else if checking}{t('Checking…')}
    {:else if error}<Icon name="alert" size={13} /> {t(error)}
    {:else if valid && mine}<Icon name="check" size={13} /> {t('An address of this wallet')}
    {:else if valid}<Icon name="check" size={13} /> {side === 'mainchain' ? t('An eCash address') : t('A valid address')}
    {/if}
  </span>
</div>

<style>
  .addr .row {
    gap: 0.35rem;
  }
  .state {
    font-size: 0.82rem;
    color: var(--text-muted);
    min-height: 1.1rem;
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
  }
  .state.bad {
    color: var(--negative);
  }
  .state.good {
    color: var(--positive);
  }
</style>
