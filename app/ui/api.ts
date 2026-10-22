// The node of a drop-in chain, as its pages see it (doc/app.md of the drop-in repositories): the host methods of its
// app layer (dropin.*), its LayerTwo reads (their own JSON), and its spends, each asked in the app's trusted dialog
// (rpc.confirmed: the node describes it, the app asks, the node runs it only on that yes). Amounts are satoshis.
import { request } from '../../lib/bridge';

export interface LinkInfo {
  slot: number;
  connected: boolean;
  error?: string;
  node: string;
  via?: string;
  height: number;
  tipheight: number;
  state?: 'ok' | 'syncing' | 'down' | 'starting';
  error_kind?: string;
  since?: number;
  behind_blocks?: number;
}

export interface Overview {
  wallet: { hasSeed: boolean; available: number; withdrawing: number; pendingIn: number; pendingOut: number };
  chain: { height: number; tip: string; peers: number; wealth: number; slot: number; network: string };
  mainchain: LinkInfo;
}

export type HistoryKind = 'receive' | 'send' | 'self' | 'deposit' | 'withdrawal' | 'bundle' | 'mined' | string;

export interface HistoryRow {
  id: string;
  kind: HistoryKind;
  txid?: string;
  m6id?: string;
  bundleStatus?: string;
  /** What the available balance changed by (withdrawals: what left it). */
  amount: number;
  received: number;
  sent: number;
  withdrawing?: number;
  mainAddress?: string;
  /** The wallet's own address it paid (where a receipt came in). */
  ownAddress?: string;
  /** BitNames: the names it brought. */
  names?: string[];
  /** BitNames: the name a reservation of the wallet commits to (only the wallet knows it). */
  reservedName?: string;
  /** A chain of assets (BitAssets): what the row moved of each, into the wallet (+) or out of it (-), exact. */
  assets?: AssetMove[];
  fee?: number;
  to: { address: string; amount: number }[];
  recipients: number;
  pending: boolean;
  height: number | null;
  time: number | null;
  approxTime: boolean;
  block?: string;
}

/** An asset a row of the activity moved: its label (its name when the wallet knows it, else its id's start) and the
 *  signed whole amount ("+1000", "-250"; exact text, an asset's amounts may pass 2^53). */
export interface AssetMove {
  label: string;
  amount: string;
  id?: string;
  assetId?: string;
  name?: string;
}

export interface OutputView {
  address: string;
  amount: number;
  /** value, withdrawal; a chain's own kinds besides (BitAssets: asset, control, lp, reservation, receipt). */
  kind: 'value' | 'withdrawal' | string;
  /** A chain of assets: the whole amount of an asset or LP output (exact text), its asset's id and name. */
  units?: string;
  asset?: string;
  assetName?: string;
  mainAddress?: string;
  mainFee?: number;
  /** Whether it is the wallet's (only for a caller that may read the wallet's addresses). */
  mine?: boolean;
  outpoint?: unknown;
  spentBy?: string;
}

export interface BlockSummary {
  height: number;
  hash: string;
  mainHash: string;
  time?: number;
  txs: number;
  inputs: number;
  outputs: number;
  fees: number;
  bytes: number;
  limit: number;
}

export interface BlockDetail extends BlockSummary {
  prevHash: string;
  merkleRoot: string;
  onChain: boolean;
  coinbase: OutputView[];
  transactions: { txid: string; inputs: number; outputs: OutputView[] }[];
}

export interface TxDetail {
  txid: string;
  pending: boolean;
  block?: string;
  height?: number;
  time?: number;
  confirmations?: number;
  inputs: ({ outpoint: unknown } | OutputView)[];
  outputs: OutputView[];
  fee?: number;
}

export interface AddressDetail {
  address: string;
  mine?: boolean;
  balance: number;
  utxos: OutputView[];
  spent: OutputView[];
  depositAddress: string;
}

export interface Withdrawal {
  txid: string;
  mainAddress: string;
  amount: number;
  mainFee: number;
  state: 'waiting' | 'in-pending-bundle' | 'submitted' | 'paid' | 'failed';
  m6id?: string;
  bundleStatus?: string;
}

export interface PegInfo {
  pendingBundle: { m6id: string; withdrawals: number; total: number } | null;
  lastFailedHeight: number | null;
  withdrawals: Withdrawal[];
  wealth: number;
  mainchain: LinkInfo | null;
  slot: number;
}

export interface SeedStatus {
  hasSeed: boolean;
  phraseKept: boolean;
  from: '' | 'app' | 'restored' | 'unified' | 'outside';
  fingerprint: string;
  derivation: string;
}

export interface Peer {
  address: string;
  connected: boolean;
  inbound: boolean;
}

export interface NodeSettings {
  network: string;
  networks: string[];
  mainchainUrl: string;
  defaultMainchainUrl: string;
  datadir: string;
  defaultDatadir: string;
  rpcPort: number;
  p2pPort: number;
  peers: string[];
  fixedByCommandLine: string[];
  log: string;
}

const host = <T>(method: string, params: Record<string, unknown> = {}) => request<T>('host', method, params);

export const dropin = {
  overview: () => host<Overview>('dropin.overview'),
  history: (limit = 500) => host<HistoryRow[]>('dropin.history', { limit }),
  newAddress: () => host<{ address: string; depositAddress: string }>('dropin.newAddress'),
  addresses: () => host<{ address: string; depositAddress: string; balance: number; received: number }[]>('dropin.addresses'),
  blocks: (count: number, from?: number) => host<BlockSummary[]>('dropin.blocks', from === undefined ? { count } : { count, from }),
  block: (at: { hash: string } | { height: number }) => host<BlockDetail>('dropin.block', at),
  transaction: (txid: string) => host<TxDetail>('dropin.transaction', { txid }),
  address: (address: string) => host<AddressDetail>('dropin.address', { address }),
  search: (query: string) => host<{ kind: 'block' | 'transaction' | 'address' | 'none'; hash?: string; txid?: string; address?: string }>('dropin.search', { query }),
  checkAddress: (address: string, side: 'chain' | 'mainchain' = 'chain') =>
    host<{ valid: boolean; mine?: boolean; error?: string }>('dropin.checkAddress', side === 'mainchain' ? { address, side: 'mainchain' } : { address }),
  peg: () => host<PegInfo>('dropin.peg'),
  peers: () => host<Peer[]>('dropin.peers'),
  connectPeer: (address: string) => host<boolean>('dropin.peers.connect', { address }),
  forgetPeer: (address: string) => host<boolean>('dropin.peers.forget', { address }),
  seedStatus: () => host<SeedStatus>('dropin.seed.status'),
  seedPrepare: () => host<{ id: string; words: number }>('dropin.seed.prepare'),
  seedCreate: (id: string) => host<{ fingerprint: string }>('dropin.seed.create', { id }),
  seedRestore: (phrase: string) => host<{ fingerprint: string }>('dropin.seed.restore', { phrase }),
  seedShow: () => host<boolean>('dropin.seed.show'),
  seedSave: (path: string) => host<boolean>('dropin.seed.save', { path }),
};

/** A spend of the wallet: described by the node, asked in the app's trusted dialog, run on a yes (BridgeError CANCELLED on a no). */
export function confirmed<T>(method: string, params: unknown[]): Promise<T> {
  return request<T>('host', 'rpc.confirmed', { method, params });
}

/** A LayerTwo read, its JSON as theirs. */
export function l2l<T>(method: string, params: unknown[] = []): Promise<T> {
  return request<T>('rpc', method, params);
}

/** The settings of the node the app runs (only in the app of the chain: not in the unified wallet). */
export const appNode = {
  get: () => host<NodeSettings>('appnode.get'),
  set: (changes: Partial<Pick<NodeSettings, 'network' | 'mainchainUrl' | 'datadir' | 'rpcPort' | 'p2pPort' | 'peers'>>) => host<NodeSettings>('appnode.set', changes),
  chooseDatadir: () => host<string | null>('appnode.chooseDatadir'),
  restart: () => host<boolean>('app.restart'),
};

/** Satoshis of the host (a JSON number, exact below 2^53) as the kit's bigint. */
export function sats(n: number | null | undefined): bigint {
  return n === null || n === undefined || !Number.isFinite(n) ? 0n : BigInt(Math.trunc(n));
}

/** A bigint amount the kit typed, as the JSON number of satoshis a LayerTwo method takes. */
export function satsNumber(v: bigint): number {
  if (v < 0n || v > 9_007_199_254_740_991n) throw new Error('amount out of range');
  return Number(v);
}
