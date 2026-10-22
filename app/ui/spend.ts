// What differs between the drop-in chains for the shared pages, from the chain's dropin.config.ts (all optional):
//   spend: the LayerTwo names of the spends Send and Drivechains make (their parameters are in the same order on every
//          chain: transfer [dest, value_sats, fee_sats], deposit [address, value_sats, fee_sats], withdraw
//          [mainchain_address, amount_sats, fee_sats, mainchain_fee_sats]); Thunder's names by default (BitAssets:
//          transfer, withdraw; zSide: create_transparent_transfer).
//   kinds: the words and icon of a chain's own kinds of activity rows (dropin.history's `kind`), in English (translated
//          when shown); optionally the other side of its line (`line`, as counterparty) and its name in the type filter.
//   rowFacts: facts of a row of the chain's own the transaction dialog shows (zSide: the memo, the move between its
//          balances), its values data (SafeText), the labels in English.
//   balanceTitle: the title of Home's balance card (zSide: the transparent balance, beside its shielded one).
//   depositNote: a word under the deposit address of Drivechains (zSide: why it is the bare address), in English.
import type { IconName } from '../../components/Icon.svelte';
import { DROPIN } from '../dropin.config';
import type { HistoryRow } from './api';

export interface SpendMethods {
  transfer: string;
  deposit: string;
  withdraw: string;
}

export interface LocalKind {
  text: string;
  icon: IconName;
  /** The other side of the row's line (counterparty), when the chain says it better. */
  line?: (row: HistoryRow) => string;
  /** Its name in the activity's type filter (none: not in the filter). */
  filter?: string;
}

export interface RowFact {
  label: string;
  value: string;
  mono?: boolean;
}

const config = DROPIN as {
  spend?: Partial<SpendMethods>;
  kinds?: Record<string, LocalKind>;
  rowFacts?: (row: HistoryRow) => RowFact[];
  balanceTitle?: string;
  depositNote?: string;
};

export const SPEND: SpendMethods = { transfer: 'create_transfer', deposit: 'create_deposit', withdraw: 'create_withdrawal', ...(config.spend ?? {}) };

export const LOCAL_KINDS: Readonly<Record<string, LocalKind>> = config.kinds ?? {};

export const rowFacts = (row: HistoryRow): RowFact[] => config.rowFacts?.(row) ?? [];

export const BALANCE_TITLE: string = config.balanceTitle ?? 'Balance';

export const DEPOSIT_NOTE: string = config.depositNote ?? '';
