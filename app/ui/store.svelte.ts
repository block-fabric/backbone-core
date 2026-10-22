// What the drop-in pages share: the node's overview, the wallet's backup-phrase status and history, read again when the
// node says something changed (a new tip, a balance, a transaction of the wallet) and every few seconds while shown.
import { on, request } from '../../lib/bridge';
import type { AppInfo } from '../../lib/rpc';
import { app } from '../../lib/state.svelte';
import { errorText } from '../../lib/dialogs.svelte';
import { dropin, type HistoryRow, type Overview, type SeedStatus } from './api';

export const drop = $state({
  overview: null as Overview | null,
  seed: null as SeedStatus | null,
  history: [] as HistoryRow[],
  historyLoaded: false,
  error: '',
  /** Bumped on every change the node told of: the pages read again what they show. */
  version: 0,
});

let refreshing: Promise<void> | null = null;
let historyWanted = 0;

/** Read the overview and the phrase's status again (one call at a time). */
export function refresh(): Promise<void> {
  if (refreshing) return refreshing;
  refreshing = (async () => {
    try {
      // The app said who it is before its node ran (app.hello at the splash): the node's facts once it runs.
      if (!(app.info as { hostApi?: number } | null)?.hostApi) app.info = await request<AppInfo>('host', 'app.info', {});
      const [o, s] = await Promise.all([dropin.overview(), dropin.seedStatus()]);
      drop.overview = o;
      drop.seed = s;
      drop.error = '';
      if (historyWanted > 0 && s.hasSeed) {
        drop.history = await dropin.history(historyWanted);
        drop.historyLoaded = true;
      } else if (!s.hasSeed) {
        drop.history = [];
        drop.historyLoaded = true;
      }
    } catch (e) {
      drop.error = errorText(e);
    } finally {
      refreshing = null;
    }
  })();
  return refreshing;
}

/** A page that shows the history wants at least `limit` rows: read now and kept fresh. */
export function wantHistory(limit: number): () => void {
  historyWanted = Math.max(historyWanted, limit);
  void refresh();
  return () => {
    historyWanted = 0;
  };
}

let followed = false;
/** Follow the node's events (once, from the app's start). */
export function follow(): void {
  if (followed) return;
  followed = true;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const soon = () => {
    if (timer) return;
    timer = setTimeout(() => {
      timer = undefined;
      drop.version++;
      void refresh();
    }, 250);
  };
  for (const type of ['tip', 'wallet.balance', 'wallet.tx', 'wallet.loaded', 'wallet.unloaded', 'connections']) on(type, soon);
  setInterval(() => void refresh(), 15_000);
  void refresh();
}

/** Shown in the unified wallet (the node is the host of the pages over RPC): the wallet's phrase and the node's settings are its. */
export function embedded(): boolean {
  return !!(app.info as { embedded?: boolean } | null)?.embedded;
}
