<script lang="ts">
  // Settings → Node: the network, the mainchain interface, the data folder, the ports and extra peers of the node the
  // app runs. Saved by the app; the node takes them when it starts again (a button here).
  import Icon from '../../components/Icon.svelte';
  import { CHAIN } from '../config';
  import { DROPIN } from '../dropin.config';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { toast } from '../../lib/state.svelte';
  import { appNode, type NodeSettings } from './api';
  import { networkLabel } from './link';
  import { embedded } from './store.svelte';

  let s = $state<NodeSettings | null>(null);
  let error = $state('');
  let changed = $state(false);
  let network = $state('');
  let url = $state('');
  let datadir = $state('');
  let rpcPort = $state('');
  let p2pPort = $state('');
  let peers = $state('');

  async function load() {
    try {
      s = await appNode.get();
      network = s.network;
      url = s.mainchainUrl;
      datadir = s.datadir;
      rpcPort = String(s.rpcPort);
      p2pPort = String(s.p2pPort);
      peers = s.peers.join('\n');
      error = '';
    } catch (e) {
      error = errorText(e);
    }
  }
  $effect(() => void load());
  const fixed = (k: string) => !!s?.fixedByCommandLine.includes(k);

  async function save() {
    try {
      s = await appNode.set({
        network,
        mainchainUrl: url.trim(),
        datadir: datadir.trim(),
        rpcPort: Number(rpcPort),
        p2pPort: Number(p2pPort),
        peers: peers
          .split(/\s+/)
          .map((p) => p.trim())
          .filter(Boolean),
      });
      changed = true;
      error = '';
      toast('success', t('Saved'), t('The node takes them when it starts again.'));
    } catch (e) {
      error = errorText(e);
    }
  }
  async function choose() {
    const dir = await appNode.chooseDatadir().catch(() => null);
    if (dir) datadir = dir;
  }
  async function restart() {
    await appNode.restart().catch((e) => toast('error', t('Restart'), errorText(e)));
  }
</script>

{#if embedded()}
  <p class="muted">{t('The node is run by the Backbone wallet: its settings are there.')}</p>
{:else}
  <form
    class="node-settings"
    onsubmit={(e) => {
      e.preventDefault();
      save();
    }}
  >
    <h2>{t('The node of %1', CHAIN.name)}</h2>
    <div class="field">
      <label for="opt-net">{t('Network')}</label>
      <select id="opt-net" bind:value={network} disabled={fixed('network')}>
        {#each s?.networks ?? [] as n (n)}<option value={n}>{networkLabel(n)}</option>{/each}
      </select>
      <span class="hint">{fixed('network') ? t('Set on the command line (--network).') : t('The node’s networks are LayerTwo’s: betanet is the one of eCash’s betanet (ecashbeta, slot %1); regtest and signet are for tests. The eCash network itself is the mainchain interface’s.', DROPIN.slot)}</span>
    </div>
    <div class="field">
      <label for="opt-url">{t('Mainchain interface (gRPC)')}</label>
      <input id="opt-url" type="text" class="mono" bind:value={url} disabled={fixed('mainchain-grpc-url')} placeholder={s?.defaultMainchainUrl} />
      <span class="hint">{t('The bip300301 enforcer’s interface: an enforcer of eCash, or Backbone started with -grpc. Default %1.', s?.defaultMainchainUrl ?? '')}</span>
    </div>
    <div class="field">
      <label for="opt-dir">{t('Data folder')}</label>
      <div class="row">
        <input id="opt-dir" type="text" class="mono grow" bind:value={datadir} disabled={fixed('datadir')} />
        <button type="button" class="btn small" disabled={fixed('datadir')} onclick={choose}><Icon name="folder" size={15} /> {t('Choose…')}</button>
      </div>
      <span class="hint">{t('LayerTwo’s %1 keeps its data in the same place by default; ours keeps its own files there.', CHAIN.name)}</span>
    </div>
    <div class="row ports">
      <div class="field">
        <label for="opt-rpc">{t('RPC port (LayerTwo’s)')}</label>
        <input id="opt-rpc" type="number" min="1024" max="65535" bind:value={rpcPort} disabled={fixed('rpc-port')} />
      </div>
      <div class="field">
        <label for="opt-p2p">{t('P2P port')}</label>
        <input id="opt-p2p" type="number" min="1024" max="65535" bind:value={p2pPort} disabled={fixed('p2p-port')} />
      </div>
    </div>
    <div class="field">
      <label for="opt-peers">{t('More peers (host:port, one a line)')}</label>
      <textarea id="opt-peers" class="mono" rows="3" bind:value={peers}></textarea>
    </div>
    {#if s?.fixedByCommandLine.length}<p class="hint">{t('Some are set on the command line, and taken from there.')}</p>{/if}
    {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}
    <div class="row">
      <button type="submit" class="btn primary">{t('Save')}</button>
      <button type="button" class="btn" onclick={restart}><Icon name="refresh" size={15} /> {changed ? t('Restart the node now') : t('Restart the node')}</button>
    </div>
  </form>
{/if}

<style>
  .node-settings {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    max-width: 44rem;
    padding-bottom: 5rem;
  }
  h2 {
    font-size: 1.05rem;
    margin: 0;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.84rem;
  }
  .ports {
    gap: 1rem;
  }
  .ports input {
    width: 9rem;
  }
  .error {
    color: var(--negative);
    margin: 0;
  }
</style>
