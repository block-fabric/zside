// The words of the trade the pages use, each with a short explanation in plain words: what <Term>
// (components/Term.svelte) shows on hover, focus or a tap. English here; t() translates them where they
// are shown. A chain may say more of its own words with addTerms() (from its chain hooks).

export interface GlossaryEntry {
  /** The word, as the pages write it. */
  term: string;
  /** What it means, in a sentence or two of plain words. */
  text: string;
}

const BASE = {
  sidechain: { term: 'sidechain', text: 'A separate blockchain with its own features (markets, names, privacy…) that uses coins moved from the main chain. They can be moved back later.' },
  mainchain: { term: 'main chain', text: 'The chain your coins start on. Sidechains borrow its coins and its miners.' },
  drivechain: { term: 'drivechain', text: 'The system that lets coins move between the main chain and its sidechains, with the miners of the main chain watching over the coins held for each sidechain.' },
  bmm: { term: 'merged mining', text: 'The miners of the main chain also make the blocks of the sidechains, in the same work, and earn the fees of the sidechains for it (blind merged mining, BMM).' },
  deposit: { term: 'deposit', text: 'Moving coins from the main chain into a sidechain. They arrive in your wallet there after a block or two.' },
  withdrawal: { term: 'withdrawal', text: 'Moving coins from a sidechain back to the main chain. It takes longer than a deposit: the miners approve withdrawals together, in bundles.' },
  bundle: { term: 'withdrawal bundle', text: 'One transaction that pays many withdrawals of a sidechain at once. The miners of the main chain vote on it over many blocks before it is paid.' },
  escrow: { term: 'escrow', text: 'The coins held on the main chain for a sidechain: everything deposited to it, minus what was withdrawn.' },
  slot: { term: 'slot', text: 'A numbered place for a sidechain on the main chain. Each sidechain has its own slot. For FastBlocks, a slot is also one short turn in which one validator makes the next block.' },
  epoch: { term: 'epoch', text: 'A run of slots after which the list of validators and their stakes is updated.' },
  validator: { term: 'validator', text: 'A node that has staked coins and takes its turn making blocks. It earns fees and rewards for it, and can lose part of its stake if it cheats.' },
  bond: { term: 'stake', text: 'Locking coins so your node can take turns making blocks and earn for it (also called bonding). You can unlock them later.' },
  unbond: { term: 'unstake', text: 'Asking for your staked coins back (unbonding). They come back after a waiting time, during which they earn nothing.' },
  finality: { term: 'final', text: 'A block or payment that can no longer be undone. Before that, a rare change of the chain could still move it.' },
  confirmation: { term: 'confirmation', text: 'Each new block after the one with your transaction is one more confirmation. The more there are, the safer the payment.' },
  mempool: { term: 'mempool', text: 'The waiting room of transactions that are sent but not yet in a block.' },
  feerate: { term: 'fee rate', text: 'How much you pay per unit of size of a transaction. A higher fee rate gets into a block sooner.' },
  fee: { term: 'fee', text: 'A small amount paid to whoever puts your transaction into a block.' },
  votecoin: { term: 'Votecoin', text: 'The reputation of Hivemind: its holders vote on what really happened, so that the markets can pay out. It can be sent like a coin.' },
  decision: { term: 'decision', text: 'A question with an answer that becomes known later (“Will it rain in Paris on May 1?”). The holders of Votecoin vote on the answer.' },
  market: { term: 'market', text: 'A place to bet on the answer of one or more questions. The price of each answer is what the traders together think its chance is.' },
  slippage: { term: 'slippage', text: 'How much the price may move against you between the quote and the trade. A larger tolerance makes the trade more likely to go through.' },
  pool: { term: 'liquidity pool', text: 'Two assets held together so that anyone can swap one for the other at a price set by how much of each is in it. Those who add to it earn the swap fees.' },
  shielded: { term: 'private', text: 'Coins held in a way that hides who sends, who receives and how much. Only you, and whoever you give a viewing key, can see them.' },
  viewingkey: { term: 'viewing key', text: 'A key that lets someone see your private payments, without being able to spend them (for your accountant, say).' },
  address: { term: 'address', text: 'Where coins are sent, like an account number. Give it to whoever pays you; a new one each time is more private.' },
  wallet: { term: 'wallet', text: 'The file on this computer that holds the keys to your coins. Keep a backup of it.' },
  block: { term: 'block', text: 'A page of the shared ledger, with the transactions of the last minute or so. Each new block confirms the ones before.' },
} as const satisfies Record<string, GlossaryEntry>;

export type TermId = keyof typeof BASE | (string & {});

const extra: Record<string, GlossaryEntry> = {};

/** Words of a chain of its own (its hooks call this when the module loads). */
export function addTerms(terms: Record<string, GlossaryEntry>): void {
  Object.assign(extra, terms);
}

/** The entry of a word (undefined: not in the glossary). */
export function glossaryEntry(id: TermId): GlossaryEntry | undefined {
  return (extra as Record<string, GlossaryEntry>)[id] ?? (BASE as Record<string, GlossaryEntry>)[id];
}

/** Every word of the glossary, by id. */
export function glossaryIds(): string[] {
  return [...new Set([...Object.keys(BASE), ...Object.keys(extra)])];
}
