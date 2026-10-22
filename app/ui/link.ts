// The link of the node to eCash (getmainchaininfo), in words and a tone.
import { t } from '../../lib/i18n.svelte';
import type { LinkInfo } from './api';

export function linkState(link: Pick<LinkInfo, 'state' | 'connected'> | null | undefined): { tone: 'good' | 'warn' | 'bad' | ''; state: string } {
  if (!link) return { tone: '', state: '' };
  const state = link.state ?? (link.connected ? 'ok' : 'down');
  return { tone: state === 'ok' ? 'good' : state === 'syncing' || state === 'starting' ? 'warn' : 'bad', state };
}

export function linkText(link: Pick<LinkInfo, 'state' | 'connected' | 'behind_blocks'> | null | undefined): string {
  const { state } = linkState(link);
  if (!state) return '–';
  if (state === 'ok') return t('in step');
  if (state === 'syncing') return link?.behind_blocks ? t('catching up (%1 blocks)', link.behind_blocks) : t('catching up');
  if (state === 'starting') return t('connecting…');
  return t('not reachable');
}

/** A network of the drop-in node (LayerTwo's names, its --network) with the eCash network it goes with. */
export function networkLabel(network: string): string {
  switch (network) {
    case 'betanet':
      return t('betanet (eCash betanet, ecashbeta)');
    case 'signet':
      return t('signet (tests)');
    case 'regtest':
      return t('regtest (local tests)');
    default:
      return network;
  }
}
