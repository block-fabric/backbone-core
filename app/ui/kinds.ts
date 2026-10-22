// The kinds of the wallet's activity (dropin.history), in words and an icon.
import type { IconName } from '../../components/Icon.svelte';
import { t } from '../../lib/i18n.svelte';
import { formatAmount } from '../../lib/units';
import { toCsv } from '../../lib/wallet';
import { sats, type HistoryRow } from './api';

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

/**
 * The activity as a CSV file. Every cell goes through the kit's toCsv (csvSafe): a name, an address or any other text
 * that someone else chose on the chain (a BitName is anyone's text) and that a spreadsheet would run as a formula (=, +,
 * -, @, a tab or a return first) gets a ' first; numbers stay numbers.
 */
export function activityCsv(rows: readonly HistoryRow[], ticker: string): string {
  return toCsv(
    ['Date', 'Type', 'Address or name', `Amount (${ticker})`, 'Fee', 'Transaction', 'Block', 'Status'],
    rows.map((r) => [
      r.time ? new Date(r.time * 1000).toISOString() : '',
      kindText(r),
      counterparty(r),
      formatAmount(sats(r.amount), 'CHN', { plus: true }),
      r.fee !== undefined ? formatAmount(sats(r.fee), 'CHN') : '',
      r.txid ?? r.m6id ?? r.id,
      r.height !== null && r.height !== undefined ? String(r.height) : '',
      r.pending ? 'pending' : (r.bundleStatus ?? 'confirmed'),
    ]),
  );
}
