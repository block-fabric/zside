import { describe, expect, it } from 'vitest';
import { bundleCanPass, bundleProgress, proposalErrors, proposalNeeds, proposalProgress, slotOverview, summaryText, type DrivechainInfo, type PendingProposal } from './chain';
import { CONSOLE_HISTORY, ConsoleHistory, commandsFromHelp, complete, completions } from './console';
import { connectionType, networkName, peerDetails, pingText, sinceText, type PeerInfo } from './peers';
import { blocksLeft, estimate } from './syncoverlay';
import {
  bareDescriptor,
  coinsToUnits,
  explorerQuery,
  formatProof,
  jsonCoinsToUnits,
  keyOfDescriptor,
  parseProof,
  proofVerdict,
  psbtStatus,
  sharedDescriptor,
  signersText,
  simplified,
  stampPayload,
  thirdPartyLinks,
  unitsToCoins,
} from './tools';
import { rangeText, rates } from './traffic.svelte';

const INFO: DrivechainInfo = {
  maxsidechains: 256,
  activationperiod: 20,
  activationmaxfailures: 9,
  replacementperiod: 40,
  withdrawalperiod: 60,
  withdrawalminscore: 30,
  maxpendingbundles: 64,
  upvoteexpiryblocks: 20,
  height: 130,
  activesidechains: 1,
  proposals: 1,
  pendingbundles: 0,
  escrowtotal: 3,
  defaultwithdrawalvote: 'upvote',
};

function proposal(over: Partial<PendingProposal> = {}): PendingProposal {
  return { slot: 2, title: 'Second', description: '', version: 0, hashid1: '0'.repeat(64), hashid2: '0'.repeat(40), proposalhash: 'aa'.repeat(32), height: 123, age: 2, acks: 5, failures: 0, blocksleft: 18, replacement: false, ack: true, ...over };
}

describe('the console', () => {
  it('keeps each line once, the newest last, at most 50, and browses them', () => {
    const h = new ConsoleHistory();
    h.add('getblockcount');
    h.add('help');
    h.add('getblockcount');
    expect(h.lines).toEqual(['help', 'getblockcount']);
    expect(h.browse(-1)).toBe('getblockcount');
    expect(h.browse(-1)).toBe('help');
    expect(h.browse(-1)).toBe('help');
    expect(h.browse(1)).toBe('getblockcount');
    expect(h.browse(1)).toBe('');
    for (let i = 0; i < 60; i++) h.add(`c${i}`);
    expect(h.lines.length).toBe(CONSOLE_HISTORY);
    expect(h.lines[0]).toBe('c10');
  });

  it('completes command names where a command goes', () => {
    const commands = commandsFromHelp('== Blockchain ==\ngetbestblockhash\ngetblock "blockhash" ( verbosity )\ngetblockcount\n\n== Wallet ==\ngetnewaddress ( "label" )');
    expect(commands).toEqual(['getbestblockhash', 'getblock', 'getblockcount', 'getnewaddress', 'help-console']);
    expect(completions('getblo', commands)).toEqual(['getblock', 'getblockcount']);
    expect(completions('getblock(getbes', commands)).toEqual(['getbestblockhash']);
    // An argument is not a command.
    expect(completions('getblock getbes', commands)).toEqual([]);
    expect(completions('', commands)).toEqual([]);
    expect(complete('getblock(getbes', 'getbestblockhash')).toBe('getblock(getbestblockhash');
  });
});

describe('peers', () => {
  const peer: PeerInfo = {
    id: 3,
    addr: '127.0.0.1:18444',
    network: 'not_publicly_routable',
    services: '0000000000000c09',
    servicesnames: ['NETWORK', 'WITNESS', 'NETWORK_LIMITED'],
    relaytxes: true,
    lastsend: 990,
    lastrecv: 995,
    last_transaction: 0,
    last_block: 900,
    bytessent: 2_500,
    bytesrecv: 1_200_000,
    conntime: 400,
    timeoffset: -1,
    pingtime: 0.0123,
    minping: 0.004,
    version: 70016,
    subver: '/Chains:32.0.0/',
    inbound: false,
    bip152_hb_to: true,
    bip152_hb_from: false,
    synced_headers: 120,
    synced_blocks: -1,
    connection_type: 'manual',
    transport_protocol_type: 'v2',
    session_id: 'ab'.repeat(32),
    permissions: [],
    addr_relay_enabled: false,
    addr_processed: 0,
    addr_rate_limited: 0,
  };

  it('names networks, types and times as chains-qt', () => {
    expect(networkName('ipv4')).toBe('IPv4');
    expect(networkName('onion')).toBe('Onion');
    expect(connectionType('outbound-full-relay', false)).toBe('Full Relay');
    expect(connectionType('outbound-full-relay', false, true)).toBe('Outbound Full Relay');
    expect(connectionType('inbound', true, true)).toBe('Inbound');
    expect(pingText(undefined)).toBe('N/A');
    expect(pingText(0.0123)).toBe('12 ms');
    expect(sinceText(1000, 0)).toBe('Never');
    expect(sinceText(1000, 940)).toBe('1 m');
  });

  it('lists the details of a peer', () => {
    const rows = Object.fromEntries(peerDetails(peer, 1000).map((r) => [r.label, r.value]));
    expect(rows['Direction/Type']).toBe('Outbound Manual');
    expect(rows['Session ID']).toBe('ab'.repeat(32));
    expect(rows['High Bandwidth']).toBe('To');
    expect(rows['Synced Blocks']).toBe('Unknown');
    expect(rows['Synced Headers']).toBe('120');
    expect(rows['Services']).toBe('Network & Witness & Network limited');
    expect(rows['Received']).toBe('1 MB');
    expect(rows['Last Transaction']).toBe('Never');
    expect(rows['Connection Time']).toBe('10 m');
    expect(rows['Time Offset']).toBe('-1 seconds');
  });
});

describe('drivechains', () => {
  it('shows what each slot holds', () => {
    const slots = slotOverview({
      info: INFO,
      sidechains: [{ slot: 1, title: 'Testchain', description: '', version: 0, hashid1: '', hashid2: '', proposalhash: '', activationheight: 121, escrowscript: 'b4010151', pendingbundles: 0 }],
      proposals: { pending: [proposal(), proposal({ slot: 1, title: 'Replacement', proposalhash: 'bb'.repeat(32), replacement: true })], queued: [{ slot: 3, title: 'Third', description: '', version: 0, hashid1: '', hashid2: '', proposalhash: 'cc'.repeat(32) }] },
    });
    expect(slots.length).toBe(256);
    expect(slots[0].state).toBe('free');
    expect(slots[1]).toEqual({ slot: 1, state: 'active', title: 'Testchain', replacing: true });
    expect(slots[2].state).toBe('proposed');
    expect(slots[3].state).toBe('queued');
  });

  it('measures proposals and bundles', () => {
    expect(proposalProgress(proposal(), INFO)).toBe(25);
    expect(proposalNeeds(proposal({ replacement: true }), INFO)).toBe(40);
    const bundle = { slot: 1, hash: 'dd'.repeat(32), score: 15, blocksleft: 10, payable: false, known: true, vote: 'upvote' };
    expect(bundleProgress(bundle, INFO)).toBe(50);
    expect(bundleCanPass(bundle, INFO)).toBe(false);
    expect(bundleCanPass({ ...bundle, blocksleft: 20 }, INFO)).toBe(true);
    expect(bundleProgress({ ...bundle, score: 40 }, INFO)).toBe(100);
  });

  it('checks a proposal before it goes to the node, and sums up the state', () => {
    expect(proposalErrors({ slot: 1, title: 'x', hash1: '', hash2: '' }, 256)).toEqual([]);
    expect(proposalErrors({ slot: 256, title: ' ', hash1: 'xyz', hash2: '12' }, 256)).toEqual([
      'The slot is a number from 0 to 255.',
      'A proposal needs a title.',
      'The release tarball hash is 64 hexadecimal characters.',
      'The build commit hash is 40 hexadecimal characters.',
    ]);
    expect(summaryText(INFO, '3.00000000 CHN')).toBe(
      '1 of 256 sidechain slots in use, holding 3.00000000 CHN in escrow. A proposal needs 20 blocks to activate and is rejected after 9 blocks without an ack. A withdrawal has 60 blocks to reach a score of 30.',
    );
  });
});

describe('tools', () => {
  it('writes and reads proofs of funds', () => {
    const text = formatProof('These coins are mine', [
      { address: 'rchn1qaaa', signature: 'SIG1' },
      { address: 'rchn1qbbb', signature: 'SIG2' },
    ]);
    expect(text).toBe('-----BEGIN CHAINS PROOF OF FUNDS-----\nThese coins are mine\n-----SIGNATURES-----\nrchn1qaaa SIG1\nrchn1qbbb SIG2\n-----END CHAINS PROOF OF FUNDS-----\n');
    const parsed = parseProof(text.replace('rchn1qbbb SIG2', 'garbage'));
    expect(parsed?.statement).toBe('These coins are mine');
    expect(parsed?.entries).toEqual([{ address: 'rchn1qaaa', signature: 'SIG1' }, { invalid: 'garbage' }]);
    expect(parseProof('hello')).toBeNull();
    expect(simplified('  a \n  b  ')).toBe('a b');
    expect(proofVerdict(2, 0, '3.00000000')).toMatch(/^VALID\. The holder of 2 address\(es\), with 3.00000000 CHN/);
    expect(proofVerdict(0, 2, '0')).toMatch(/^NOT VALID\./);
    expect(proofVerdict(1, 1, '1.00000000')).toMatch(/^PARTLY VALID\. 1 signature\(s\) are good/);
  });

  it('counts multisig amounts exactly', () => {
    expect(coinsToUnits('1.5')).toBe(150_000_000n);
    expect(coinsToUnits('0.00000001')).toBe(1n);
    expect(coinsToUnits('1.123456789')).toBeNull();
    expect(coinsToUnits('abc')).toBeNull();
    expect(unitsToCoins(199_990_000n)).toBe('1.99990000');
    expect(unitsToCoins(-5n)).toBe('-0.00000005');
    expect(jsonCoinsToUnits(0.1 + 0.2)).toBe(30_000_000n);
    const d = sharedDescriptor(2, ['[abcd/84h/1h/0h/0/1]02aa', '[abcd/84h/1h/0h/0/2]03bb']);
    expect(d).toBe('wsh(sortedmulti(2,[abcd/84h/1h/0h/0/1]02aa,[abcd/84h/1h/0h/0/2]03bb))');
    expect(signersText(`${d}#checksum`)).toBe('2 of 2');
    expect(bareDescriptor(`${d}#checksum`)).toBe(d);
    expect(keyOfDescriptor('wpkh([abcd/84h/1h/0h/0/1]02aa)#xyz')).toBe('[abcd/84h/1h/0h/0/1]02aa');
    expect(keyOfDescriptor('tr(02aa)#xyz')).toBe('');
  });

  it('says how far a shared spend is', () => {
    const v0 = {
      tx: { vout: [{ value: 1, scriptPubKey: { address: 'rchn1qdest' } }, { value: 1.9999, scriptPubKey: { address: 'rchn1qshared' } }] },
      inputs: [{ partial_signatures: { a: 'x' }, witness_script: { asm: '2 02aa 03bb 2 OP_CHECKMULTISIG' } }],
      fee: 0.0001,
    };
    expect(psbtStatus(v0)).toBe('Signatures: 1 of the 2 needed. Pays 1.00000000 CHN to rchn1qdest, and 1.99990000 CHN to rchn1qshared. Fee: 0.00010000 CHN.');
    const v2 = { outputs: [{ amount: 0.5, script: { address: 'rchn1qx' } }], inputs: [{ final_scriptwitness: ['00'] }] };
    expect(psbtStatus(v2)).toBe('Fully signed, ready to send. Pays 0.50000000 CHN to rchn1qx. Fee: ? CHN.');
  });

  it('tells what a search of the explorer is', () => {
    expect(explorerQuery(' 120 ')).toEqual({ kind: 'height', height: 120 });
    expect(explorerQuery('AB'.repeat(32))).toEqual({ kind: 'hash', hash: 'ab'.repeat(32) });
    expect(explorerQuery('1234567890')).toEqual({ kind: 'invalid' });
    expect(explorerQuery('xyz')).toEqual({ kind: 'invalid' });
  });

  it('stamps files with "STAMP" before the hash, and links transactions to explorers', () => {
    expect(stampPayload('AB'.repeat(32))).toBe('5354414d50' + 'ab'.repeat(32));
    expect(thirdPartyLinks('https://explorer.example/tx/%s | not a url | ftp://x/%s', 'abc')).toEqual([{ label: 'explorer.example', url: 'https://explorer.example/tx/abc' }]);
    expect(thirdPartyLinks('', 'abc')).toEqual([]);
  });
});

describe('traffic and sync', () => {
  it('turns byte totals into rates', () => {
    const now = 1_000_000;
    const samples = Array.from({ length: 11 }, (_, i) => ({ t: now - (10 - i) * 1000, recv: i * 2000, sent: i * 1000 }));
    const points = rates(samples, 5, now);
    expect(points.length).toBe(10);
    expect(points[0].inRate).toBe(2);
    expect(points[0].outRate).toBe(1);
    expect(points[points.length - 1].age).toBe(0);
    // Cleared: older samples are not shown.
    expect(rates(samples, 5, now, now - 3000).length).toBe(3);
    expect(rangeText(30)).toBe('30 m');
    expect(rangeText(65)).toBe('1 h 5 m');
    expect(rangeText(1440)).toBe('24 h');
  });

  it('estimates the time left of a sync', () => {
    const now = 10_000_000;
    expect(estimate([{ t: now, progress: 0.5 }], now).perHour).toBe('calculating…');
    const e = estimate(
      [
        { t: now, progress: 0.5 },
        { t: now - 3_600_000, progress: 0.4 },
      ],
      now,
    );
    expect(e.perHour).toBe('10.00%');
    expect(e.timeLeft).toBe('5 hours');
    expect(blocksLeft(100, 110, 1000, 600, 1600)).toBe('10');
    expect(blocksLeft(100, 200, 1000, 600, 1000 + 600 * 100)).toBe('Unknown. Syncing Headers (200, 66.7%)…');
  });
});
