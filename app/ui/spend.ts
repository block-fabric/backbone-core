// What differs between the drop-in chains for the shared pages, from the chain's dropin.config.ts (all optional):
//   spend: the LayerTwo names of the spends Send and Drivechains make (their parameters are in the same order on every
//          chain: transfer [dest, value_sats, fee_sats], deposit [address, value_sats, fee_sats], withdraw
//          [mainchain_address, amount_sats, fee_sats, mainchain_fee_sats]); Thunder's names by default (BitAssets:
//          transfer, withdraw; zSide: create_transparent_transfer).
//   kinds: the words and icon of a chain's own kinds of activity rows (dropin.history's `kind`), in English (translated
//          when shown).
import type { IconName } from '../../components/Icon.svelte';
import { DROPIN } from '../dropin.config';

export interface SpendMethods {
  transfer: string;
  deposit: string;
  withdraw: string;
}

export interface LocalKind {
  text: string;
  icon: IconName;
}

const config = DROPIN as { spend?: Partial<SpendMethods>; kinds?: Record<string, LocalKind> };

export const SPEND: SpendMethods = { transfer: 'create_transfer', deposit: 'create_deposit', withdraw: 'create_withdrawal', ...(config.spend ?? {}) };

export const LOCAL_KINDS: Readonly<Record<string, LocalKind>> = config.kinds ?? {};
