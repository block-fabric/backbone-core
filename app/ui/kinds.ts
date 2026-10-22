// The kinds of the wallet's activity (dropin.history), in words and an icon.
import type { IconName } from '../../components/Icon.svelte';
import { t } from '../../lib/i18n.svelte';
import type { HistoryRow } from './api';

export function kindText(row: Pick<HistoryRow, 'kind' | 'bundleStatus'>): string {
  switch (row.kind) {
    case 'receive':
      return t('Received');
    case 'send':
      return t('Sent');
    case 'self':
      return t('To yourself');
    case 'deposit':
      return t('Deposit from eCash');
    case 'withdrawal':
      return t('Withdrawal to eCash');
    case 'bundle':
      return row.bundleStatus === 'Confirmed' ? t('Paid on eCash') : row.bundleStatus === 'Failed' ? t('Bundle failed') : t('In a withdrawal bundle');
    case 'mined':
      return t('Block fees');
    case 'reservation':
      return t('Name reserved');
    case 'name':
      return t('Name registered');
    case 'name-received':
      return t('Name received');
    default:
      return row.kind;
  }
}

export function kindIcon(row: Pick<HistoryRow, 'kind'>): IconName {
  switch (row.kind) {
    case 'receive':
      return 'receive';
    case 'send':
      return 'send';
    case 'deposit':
      return 'down';
    case 'withdrawal':
    case 'bundle':
      return 'up';
    case 'mined':
      return 'block';
    case 'reservation':
    case 'name':
    case 'name-received':
      return 'book';
    default:
      return 'arrow';
  }
}

/** The other side of a row, for its line: who was paid, or the eCash address of a withdrawal. */
export function counterparty(row: HistoryRow): string {
  if (row.kind === 'withdrawal' || row.kind === 'bundle') return row.mainAddress ?? row.m6id ?? '';
  // A name's row: the name, in words when the wallet knows them.
  if (row.names?.length) return row.names.join(', ');
  if (row.reservedName) return row.reservedName;
  // What came in: the wallet's own address it came to.
  if (row.kind === 'receive' || row.kind === 'deposit' || row.kind === 'mined' || row.kind === 'self') return row.ownAddress ?? '';
  if (row.to.length === 1) return row.to[0].address;
  if (row.recipients > 1) return t('%1 recipients', row.recipients);
  return row.ownAddress ?? '';
}
