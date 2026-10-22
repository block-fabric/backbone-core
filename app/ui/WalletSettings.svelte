<script lang="ts">
  // Settings → Wallet: the backup phrase of this chain's wallet: shown again (in the app's trusted dialog), saved to a
  // file the user chooses (written by the node, never through the page), or set up when there is none. And what
  // protects it: like LayerTwo's own wallet, the keys are kept unencrypted in the data folder.
  import Icon from '../../components/Icon.svelte';
  import { CHAIN } from '../config';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { chooseFile } from '../../lib/files';
  import { t } from '../../lib/i18n.svelte';
  import { toast } from '../../lib/state.svelte';
  import SetupWallet from './SetupWallet.svelte';
  import { dropin } from './api';
  import { drop, follow } from './store.svelte';

  $effect(() => follow());
  const s = $derived(drop.seed);
  let error = $state('');

  async function show() {
    error = '';
    try {
      await dropin.seedShow();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    }
  }
  async function save() {
    error = '';
    try {
      const path = await chooseFile({ save: true, title: t('Save the backup phrase'), name: `${CHAIN.bin}-backup-phrase.txt`, filters: [{ name: 'Text', patterns: ['*.txt'] }] });
      if (!path) return;
      await dropin.seedSave(path);
      toast('success', t('Saved'), path);
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    }
  }
  const fromText = $derived(
    s?.from === 'app' ? t('made in this app') : s?.from === 'restored' ? t('restored in this app') : s?.from === 'unified' ? t('made from your Backbone backup phrase') : t('set outside this app'),
  );
</script>

<div class="wallet-settings">
  <h2>{t('Backup phrase')}</h2>
  {#if s && !s.hasSeed}
    <SetupWallet />
  {:else if s}
    <dl class="facts">
      <dt>{t('Phrase')}</dt><dd>{fromText}</dd>
      <dt>{t('Fingerprint')}</dt><dd class="mono">{s.fingerprint}</dd>
      <dt>{t('Keys')}</dt><dd>{t('LayerTwo’s %1 derivation (%2)', CHAIN.name, s.derivation)}</dd>
    </dl>
    {#if s.phraseKept}
      <div class="row">
        <button type="button" class="btn" onclick={show}><Icon name="eye" size={15} /> {t('Show the phrase…')}</button>
        <button type="button" class="btn" onclick={save}><Icon name="download" size={15} /> {t('Save it to a file…')}</button>
      </div>
      <p class="hint">{t('Shown only in the app’s own window, never to its pages. Make sure nobody can see your screen.')}</p>
    {:else}
      <p class="hint">{t('This wallet’s phrase was set outside this app (LayerTwo’s wallet, or its RPC): keep the backup you made then.')}</p>
    {/if}
  {/if}
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}
  <div class="note">
    <Icon name="lock" size={16} />
    <p>{t('As in LayerTwo’s %1, the wallet’s keys are kept in its data folder without a password: anyone with access to this computer’s account can spend them. Keep only what you need here.', CHAIN.name)}</p>
  </div>
</div>

<style>
  .wallet-settings {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    max-width: 48rem;
    padding-bottom: 5rem;
  }
  h2 {
    font-size: 1.05rem;
    margin: 0;
  }
  .facts {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.4rem 1.2rem;
    margin: 0;
  }
  .facts dt {
    color: var(--text-muted);
  }
  .facts dd {
    margin: 0;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.85rem;
    margin: 0;
  }
  .error {
    color: var(--negative);
    margin: 0;
  }
  .note {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
    padding: 0.75rem 0.9rem;
    border-radius: 0.6rem;
    background: color-mix(in srgb, var(--warning) 10%, transparent);
    color: var(--text);
    font-size: 0.88rem;
  }
  .note p {
    margin: 0;
  }
</style>
