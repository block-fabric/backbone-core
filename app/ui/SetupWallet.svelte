<script lang="ts">
  // The wallet's backup phrase, made or restored. Its words are shown only in the app's own trusted dialog (the node
  // makes the phrase and keeps it; the page gets an id, never a word): written down there, then the wallet takes it. A
  // phrase restored is typed here, checked by the node, and the wallet takes it after a question of the dialog.
  import Icon from '../../components/Icon.svelte';
  import { CHAIN } from '../config';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { toast } from '../../lib/state.svelte';
  import { dropin } from './api';
  import { embedded as isEmbedded, refresh } from './store.svelte';

  let { compact = false }: { compact?: boolean } = $props();

  let mode: 'choose' | 'restore' = $state('choose');
  let phrase = $state('');
  let busy = $state(false);
  let error = $state('');
  const words = $derived(phrase.trim() ? phrase.trim().split(/\s+/).length : 0);
  /** Shown in the unified wallet (its host is the node, over RPC): one backup phrase covers this chain there. */
  const embedded = $derived(isEmbedded());

  async function create() {
    busy = true;
    error = '';
    try {
      const { id } = await dropin.seedPrepare();
      const r = await dropin.seedCreate(id);
      toast('success', t('Your wallet is ready'), t('Fingerprint %1', r.fingerprint));
      await refresh();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }

  async function restore() {
    busy = true;
    error = '';
    try {
      const r = await dropin.seedRestore(phrase.trim().toLowerCase().split(/\s+/).join(' '));
      phrase = '';
      toast('success', t('Wallet restored'), t('Fingerprint %1', r.fingerprint));
      await refresh();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<section class="panel setup" class:compact aria-labelledby="setup-title">
  <div class="panel-body">
    <div class="lead">
      <span class="mark"><Icon name="key" size={22} /></span>
      <div>
        <h2 id="setup-title">{t('Set up your %1 wallet', CHAIN.name)}</h2>
        <p class="muted">
          {t('Your coins on %1 belong to a backup phrase: 12 words that are the only way to get them back. It is the same kind of phrase as LayerTwo’s %1 wallet uses, so either can restore the other.', CHAIN.name)}
        </p>
        {#if embedded}
          <p class="note"><Icon name="info" size={14} /> {t('In the Backbone wallet, your one backup phrase makes this one for you, and a phrase is restored there (Settings → Backup phrase), its words typed only in its own window. Make one here only for a wallet of its own.')}</p>
        {/if}
      </div>
    </div>

    {#if mode === 'choose'}
      <div class="choices">
        <button type="button" class="choice" disabled={busy} onclick={create}>
          <Icon name="plus" size={20} />
          <span><strong>{t('Make a new backup phrase')}</strong><small>{t('Shown once in the app’s own window, to write down on paper.')}</small></span>
        </button>
        {#if !embedded}
          <!-- In the unified wallet a phrase is typed only in its own window (never in a page): restored there. -->
          <button type="button" class="choice" disabled={busy} onclick={() => (mode = 'restore')}>
            <Icon name="refresh" size={20} />
            <span><strong>{t('Restore from my backup phrase')}</strong><small>{t('12 to 24 words, from this app or from LayerTwo’s wallet.')}</small></span>
          </button>
        {/if}
      </div>
    {:else}
      <form
        class="restore"
        onsubmit={(e) => {
          e.preventDefault();
          restore();
        }}
      >
        <label for="restore-words">{t('Your backup phrase')}</label>
        <textarea id="restore-words" class="mono" rows="3" spellcheck="false" autocomplete="off" bind:value={phrase} placeholder={t('word1 word2 word3 …')}></textarea>
        <span class="hint">{words ? t('%1 words', words) : t('The words, in order, separated by spaces.')}</span>
        <div class="row">
          <button type="submit" class="btn primary" disabled={busy || words < 12}><Icon name="check" size={16} /> {t('Restore')}</button>
          <button type="button" class="btn" disabled={busy} onclick={() => ((mode = 'choose'), (phrase = ''), (error = ''))}>{t('Back')}</button>
        </div>
      </form>
    {/if}
    {#if error}<p class="error" role="alert"><Icon name="alert" size={14} /> {error}</p>{/if}
  </div>
</section>

<style>
  .setup {
    border-color: color-mix(in srgb, var(--accent) 45%, var(--border));
    background: linear-gradient(135deg, color-mix(in srgb, var(--accent) 9%, var(--surface)), var(--surface) 60%);
  }
  .panel-body {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .lead {
    display: flex;
    gap: 0.9rem;
    align-items: flex-start;
  }
  .lead h2 {
    margin: 0 0 0.3rem;
    font-size: 1.15rem;
  }
  .lead p {
    margin: 0;
    max-width: 52rem;
  }
  .mark {
    display: grid;
    place-items: center;
    width: 2.6rem;
    height: 2.6rem;
    border-radius: 0.8rem;
    background: color-mix(in srgb, var(--accent) 16%, transparent);
    color: var(--accent);
    flex: none;
  }
  .note {
    margin-top: 0.5rem !important;
    color: var(--text-muted);
    font-size: 0.88rem;
    display: flex;
    gap: 0.35rem;
    align-items: flex-start;
  }
  .choices {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(16rem, 1fr));
    gap: 0.75rem;
  }
  .choice {
    display: flex;
    gap: 0.75rem;
    align-items: flex-start;
    text-align: start;
    padding: 0.85rem 1rem;
    border-radius: 0.75rem;
    border: 1px solid var(--border);
    background: var(--surface-raised, var(--surface));
    color: var(--text);
    cursor: pointer;
  }
  .choice:hover:not(:disabled) {
    border-color: var(--accent);
  }
  .choice span {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
  }
  .choice small {
    color: var(--text-muted);
  }
  .restore {
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    max-width: 40rem;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.84rem;
  }
  .error {
    color: var(--negative);
    margin: 0;
    display: flex;
    gap: 0.35rem;
    align-items: center;
  }
  .compact .lead p.muted {
    display: none;
  }
</style>
