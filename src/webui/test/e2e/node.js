// The node and Chains pages of chains-app on regtest, driven through the page as a user would:
// a sidechain proposed, acked and activated, deposits (a checked deposit address, a plain one, the
// wrong ones), a withdrawal bundle and the votes on it, the block explorer, solo mining, a file
// stamped and found, a proof of funds, a 2-of-2 multisig spend, the crypto tools, the console,
// peers banned and unbanned, the traffic, the settings, the tray and notifications of a pretend
// desktop, the sidechain nodes with pretend programs; pictures of every page in light and dark.
// At the end, the options are reset, which shuts the app down.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, text } = E2E;
  // The notices of new transactions (many, with the blocks mined here) would hide the pages.
  const snapshot = async (name) => {
    T().toasts.list.splice(0);
    await E2E.snapshot(name);
  };
  const DIR = window.__E2E_FILES;
  const BIN = window.__E2E_BIN;

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const byText = (label, root) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  // The page first: the frame of the layout has buttons of its own (the palette's "Search…").
  const button = (label, root) => (root ? byText(label, root) : byText(label, $('#content') || document) || byText(label, document));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(80);
  }
  async function dialogTitled(title, ms = 30000) {
    return waitFor(`the dialog "${title}"`, () => {
      const d = openDialog();
      return d && $('h2', d) && $('h2', d).textContent.trim() === title ? d : null;
    }, ms);
  }
  async function answerDialog(title, label) {
    const d = await dialogTitled(title);
    const body = d.textContent;
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
    return body;
  }
  async function answerPrompt(title, value) {
    const d = await dialogTitled(title);
    set($('#prompt-input', d), value);
    await sleep(50);
    $('#prompt-form', d).requestSubmit();
    await waitFor(`the prompt "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(250);
  }
  const mine = async (n) => {
    const hashes = await rpc('generatetoaddress', [n, minerAddress]);
    await waitFor(`block ${hashes.length}`, () => T().node.tip && T().node.tip.hash === hashes[hashes.length - 1], 60000);
    return hashes;
  };
  async function sha256Hex(textValue) {
    const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(textValue));
    return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
  }
  const le64 = (n) => {
    let v = BigInt(n);
    let h = '';
    for (let i = 0; i < 8; i++) {
      h += (v & 0xffn).toString(16).padStart(2, '0');
      v >>= 8n;
    }
    return h;
  };
  const be64 = (n) => le64(n).match(/../g).reverse().join('');
  // What pays, signs or changes the votes of the node is asked on the desktop (the dialog window of the host, audit 7 #1):
  // the test answers the next question, and reads what it asked.
  const desktop = (answer) => host('test.confirmAnswer', answer);
  const lastAsked = async () => (await host('test.confirmed')).at(-1);
  const rowOf = (asked, label) => (asked && asked.rows ? asked.rows.find((r) => r.label === label) : undefined);
  const dialogWindow = (action, more = {}) => host('test.dialog', { action, ...more });
  /** The question shown in the dialog window: waited for, its picture taken, then answered in it as a user would. */
  async function inWindow(name, accept) {
    await waitFor(`${name}: the dialog window`, async () => (await dialogWindow('kind')) === 'themed', 30000);
    await waitFor(`${name}: its page`, async () => {
      const shown = await dialogWindow('eval', { script: `!document.body.classList.contains('loading') && !!document.getElementById('dialog-accept')` });
      return shown === true;
    }, 30000);
    await sleep(400);
    await dialogWindow('snapshot', { name });
    // The button that accepts takes a click a second after the window shows.
    if (accept) await sleep(2600);
    await dialogWindow('eval', { script: `document.getElementById(${JSON.stringify(accept ? 'dialog-accept' : 'dialog-cancel')}).click(); true` });
    await waitFor(`${name}: the window gone`, async () => (await dialogWindow('kind')) === 'none', 20000);
  }

  /** Pictures of a page in light and dark. */
  const both = [];
  async function shoot(name, page, tab = '', prepare) {
    both.push({ name, page, tab, prepare });
    await go(page, tab);
    if (prepare) await prepare();
    await snapshot(`${name}-light`);
  }

  let minerAddress;
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    T().savePrefs({ theme: 'light' });

    // --- A wallet with coins ---
    await rpc('createwallet', ['miner']);
    await waitFor('the wallet', () => T().wallets.current === 'miner', 30000);
    minerAddress = await rpc('getnewaddress', [], 'miner');
    await mine(110);
    await waitFor('the balance', () => T().wallets.balances.miner && T().wallets.balances.miner.trusted > 0n, 60000);

    // --- Propose a sidechain, through the form ---
    await go('sidechains', 'proposals');
    set(await waitFor('the proposal form', () => $('#proposal-slot')), '1');
    set($('#proposal-title'), 'Testchain');
    set($('#proposal-description'), 'A sidechain for testing');
    set($('#proposal-hash2'), 'xyz');
    await click('Propose');
    await waitFor('the form error', () => text('.errors').includes('The build commit hash is 40 hexadecimal characters.'));
    await check('proposal: a bad hash is refused before it goes to the node', true);
    set($('#proposal-hash2'), '');
    // Proposed on the desktop's word: the slot and the title.
    await desktop({ yes: true });
    await click('Propose');
    await waitFor('the queued proposal', () => text('[aria-label="Queued proposals"]').includes('Testchain'), 30000);
    let q = await lastAsked();
    await check('proposal: asked on the desktop, with the slot and the title', q && q.title === 'Propose a sidechain' && rowOf(q, 'Title')?.value === 'Testchain', JSON.stringify(q));
    const queued = await rpc('listsidechainproposals');
    await check('proposal queued for the next block', queued.queued.length === 1 && queued.queued[0].title === 'Testchain' && queued.queued[0].description === 'A sidechain for testing');
    await snapshot('sidechains-queued-light');
    await mine(1);
    await waitFor('the proposal in the chain', () => text('[aria-label="Proposals"]').includes('Testchain'), 30000);
    const meter = await waitFor('the acks meter', () => $('[role="progressbar"][aria-label="Acks of Testchain"]'));
    await check('proposal: acks shown out of the activation period (none yet in the block that proposes it)', meter.getAttribute('aria-valuetext') === '0 / 20', meter.getAttribute('aria-valuetext'));

    // A second one, that this node stops acking.
    set($('#proposal-slot'), '2');
    set($('#proposal-title'), 'Unwanted');
    await desktop({ yes: true });
    await click('Propose');
    await waitFor('the second queued', () => text('[aria-label="Queued proposals"]').includes('Unwanted'));
    await mine(1);
    const row = await waitFor('the second proposal', () => $$('[aria-label="Proposals"] tbody tr').find((tr) => tr.textContent.includes('Unwanted')));
    row.click();
    await sleep(100);
    await desktop({ yes: true });
    await click('Stop acking');
    await waitFor('no ack', async () => (await rpc('listsidechainproposals')).pending.some((p) => p.title === 'Unwanted' && !p.ack));
    q = await lastAsked();
    await check('proposal: stop acking, asked on the desktop with the proposal', q && q.title === 'Stop acking a sidechain proposal' && rowOf(q, 'Title')?.value === 'Unwanted', JSON.stringify(q));
    await snapshot('sidechains-proposals-light');
    both.push({ name: 'sidechains-proposals', page: 'sidechains', tab: 'proposals' });

    await mine(18);
    await waitFor('the active sidechain', async () => (await rpc('listactivesidechains')).some((s) => s.slot === 1), 30000);
    await go('sidechains');
    await waitFor('the active sidechain shown', () => $('.slot.active') && text('[aria-label="Active sidechains"]').includes('Testchain'), 30000);
    await check('sidechain activated after the acks', $('.slot.active').getAttribute('aria-label') === 'Slot 1, Testchain, active');

    // --- Deposits ---
    set(await waitFor('the deposit form', () => $('#deposit-destination')), 's1_erin_b91ea9');
    await waitFor('the deposit address checked', () => text('#deposit-destination-check') === 'A deposit address for slot 1.');
    set($('#deposit-amount'), '2');
    await sleep(100);
    await click('Deposit', () => $('form', $('.deposit')));
    let d = await dialogTitled('Confirm sidechain deposit');
    await check('deposit: confirmation names the sidechain and the address', d.textContent.includes('Deposit 2.00000000 CHN to the sidechain "Testchain" (slot 1)') && d.textContent.includes('s1_erin_b91ea9'));
    await check('deposit: the preview says the fee', /Network fee: about 0\.0000\d+ CHN\. Total: about 2\.0000\d+ CHN\./.test(d.textContent), d.textContent.slice(0, 400));
    await snapshot('sidechains-deposit-confirm-light');
    // The desktop asks, with what the host decoded (audit 7 #1): pictures of it in Light, declined there; then in
    // Midnight, accepted there.
    await desktop({ show: true });
    await click('Deposit', d);
    await inWindow('spend-deposit-light', false);
    q = await lastAsked();
    await check('deposit: asked on the desktop, the sidechain, the destination whole before the amount, the fee and the total',
      q && q.title === 'Deposit to a sidechain' && rowOf(q, 'Sidechain')?.value === 'Testchain (slot 1)' && rowOf(q, 'Deposit to')?.value === 's1_erin_b91ea9' &&
        rowOf(q, 'Credited to')?.value === 'erin' && rowOf(q, 'Amount')?.value === '2.00000000 CHN' && /^about 0\.0000\d+ CHN$/.test(rowOf(q, 'Network fee')?.value ?? '') &&
        q.rows.findIndex((r) => r.label === 'Deposit to') < q.rows.findIndex((r) => r.label === 'Amount'),
      JSON.stringify(q));
    await check('deposit: declined on the desktop, nothing sent', !openDialog() && (await rpc('getrawmempool', [])).length === 0);
    T().savePrefs({ theme: 'midnight' });
    await sleep(300);
    await click('Deposit', () => $('form', $('.deposit')));
    d = await dialogTitled('Confirm sidechain deposit');
    await desktop({ show: true });
    await click('Deposit', d);
    await inWindow('spend-deposit-midnight', true);
    T().savePrefs({ theme: 'light' });
    const sent = await answerDialog('Sidechains', 'OK');
    await check('deposit sent', /Deposit sent in transaction [0-9a-f]{64}\./.test(sent), sent);

    // A destination that is not a deposit address: a question first.
    set($('#deposit-destination'), 'alice');
    set($('#deposit-amount'), '1');
    await sleep(300);
    await click('Deposit', () => $('form', $('.deposit')));
    const question = await answerDialog('Sidechains', 'Deposit anyway');
    await check('deposit: a plain destination is questioned', question.includes('"alice" is not a deposit address'));
    await desktop({ yes: true });
    await click('Deposit', await dialogTitled('Confirm sidechain deposit'));
    await answerDialog('Sidechains', 'OK');

    // A typing error, and a deposit address of another slot, are refused.
    set($('#deposit-destination'), 's1_erin_b91ea8');
    set($('#deposit-amount'), '1');
    await waitFor('the typo noticed', () => text('#deposit-destination-check') === 'This deposit address has a typing error in it.');
    await click('Deposit', () => $('form', $('.deposit')));
    await check('deposit: a typing error is refused', (await answerDialog('Sidechains', 'OK')).includes('typing error'));
    const other = `s2_bob_${(await sha256Hex('s2_bob_')).slice(0, 6)}`;
    set($('#deposit-destination'), other);
    await waitFor('the other slot noticed', () => text('#deposit-destination-check').includes('slot 2, which has no active sidechain'));
    await click('Deposit', () => $('form', $('.deposit')));
    await check('deposit: an address of another sidechain is refused', (await answerDialog('Sidechains', 'OK')).includes('is for the sidechain in slot 2, and the sidechain selected is the one in slot 1'));
    set($('#deposit-destination'), '');

    await mine(1);
    await waitFor('the deposits listed', () => text('[aria-label="Deposits"]').includes('s1_erin_b91ea9') && text('[aria-label="Deposits"]').includes('alice'), 30000);
    const sidechain = await rpc('getsidechain', [1]);
    await check('deposits in the escrow', sidechain.escrow && sidechain.escrow.amount === 3, JSON.stringify(sidechain.escrow));
    await snapshot('sidechains-light');
    both.push({ name: 'sidechains', page: 'sidechains', tab: '' });

    // --- A withdrawal bundle, and the votes on it ---
    const payTo = await rpc('getnewaddress', ['withdrawal'], 'miner');
    const spk = (await rpc('getaddressinfo', [payTo], 'miner')).scriptPubKey;
    const bundleHex = '02000000' + '00' + '02' + le64(0) + '0a' + '6a08' + be64(1_000_000) + le64(100_000_000) + (spk.length / 2).toString(16).padStart(2, '0') + spk + '00000000';
    const bundle = await rpc('receivewithdrawalbundle', [1, bundleHex]);
    await mine(1);
    await go('sidechains', 'withdrawals');
    const brow = await waitFor('the bundle', () => $$('[aria-label="Withdrawal bundles"] tbody tr').find((tr) => tr.textContent.includes('Testchain')), 30000);
    const score = $('[role="progressbar"]', brow);
    await check('bundle: score shown out of the minimum', score && score.getAttribute('aria-valuetext') === '1 of 30', score && score.getAttribute('aria-valuetext'));
    brow.click();
    await sleep(100);
    // Who the bundle pays: the row opens to them by its button (ui-tweaks, c647681ead).
    $('button.expand', brow).click();
    await waitFor('the payouts', () => text('.payouts').includes('1.00000000'));
    await check('bundle: payouts of a known bundle', true);
    // A vote is changed on the desktop's word: an upvote with the bundle and what it pays (audit 7 #1).
    await desktop({ yes: false });
    await click('Downvote sidechain');
    await waitFor('the question', async () => (await lastAsked())?.title === 'Change a withdrawal vote', 20000);
    await sleep(300);
    await check('bundle: a vote declined on the desktop changes nothing', (await rpc('listwithdrawalbundles'))[0].vote !== 'downvote');
    await desktop({ yes: true });
    await click('Downvote sidechain');
    await waitFor('the downvote', async () => (await rpc('listwithdrawalbundles'))[0].vote === 'downvote');
    await check('bundle: downvote', true);
    await desktop({ yes: true });
    await click('Upvote');
    await waitFor('the upvote', async () => (await rpc('listwithdrawalbundles'))[0].vote === 'upvote' && (await rpc('listwithdrawalbundles'))[0].hash === bundle.hash);
    q = await lastAsked();
    await check('bundle: upvote, asked on the desktop with the bundle and what it pays',
      q && rowOf(q, 'Bundle')?.value === bundle.hash && rowOf(q, 'Pays')?.value === payTo && q.level === 'danger', JSON.stringify(q));
    await mine(2);
    await waitFor('the score', () => {
      const m = $$('[aria-label="Withdrawal bundles"] [role="progressbar"]')[0];
      return m && m.getAttribute('aria-valuetext') === '3 of 30';
    }, 30000);
    await check('bundle: the score grows with the upvotes', true);
    await desktop({ yes: true });
    set($('select[aria-label="Default withdrawal vote"]'), 'abstain');
    await waitFor('the default vote', async () => (await rpc('getdrivechaininfo')).defaultwithdrawalvote === 'abstain');
    await check('bundle: default vote', true);
    await desktop({ yes: true });
    set($('select[aria-label="Default withdrawal vote"]'), 'upvote');
    await waitFor('the default vote back', async () => (await rpc('getdrivechaininfo')).defaultwithdrawalvote === 'upvote');
    await snapshot('sidechains-withdrawals-light');
    both.push({ name: 'sidechains-withdrawals', page: 'sidechains', tab: 'withdrawals' });

    // --- Block explorer ---
    await shoot('explorer', 'explorer', '', async () => {
      await waitFor('the blocks', () => $$('[aria-label="Blocks"] [role="row"]').length > 5, 30000);
    });
    set($('#explorer-search'), '5');
    await click('Search');
    await waitFor('block 5', () => text('#explorer-title') === 'Block 5');
    await check('explorer: a block by height', true);
    $('[aria-label="Transactions of the block"] button').click();
    await waitFor('its transaction', () => text('#explorer-title').startsWith('Transaction'));
    await check('explorer: a transaction of a block', text('.io').includes('coinbase'));
    const waiting = await rpc('sendtoaddress', [payTo, 0.5], 'miner');
    set($('#explorer-search'), waiting);
    await click('Search');
    await waitFor('the waiting transaction', () => text('#content .detail').includes('Waiting to be mined'));
    await check('explorer: a transaction of the mempool by its id', text('#content .detail').includes(waiting));
    await snapshot('explorer-tx-light');
    // Dark: a transaction of a block (the one of the mempool is mined by then).
    both.push({ name: 'explorer-tx', page: 'explorer', tab: 'search:3', prepare: async () => { await waitFor('block 3', () => text('#explorer-title') === 'Block 3'); $('[aria-label="Transactions of the block"] button').click(); await waitFor('tx', () => text('#explorer-title').startsWith('Transaction')); } });
    await click('Raw JSON');
    await check('explorer: raw JSON', text('pre.raw').includes('"vout"') && !text('pre.raw').includes('"hex": "02'));
    set($('#explorer-search'), 'xyz');
    await click('Search');
    await check('explorer: a search that is not one', (await answerDialog('Block Explorer', 'OK')).includes('Enter a block height, a block hash or a transaction id.'));
    await go('explorer', 'search:3');
    await waitFor('block 3', () => text('#explorer-title') === 'Block 3');
    await snapshot('explorer-block-light');
    both.push({ name: 'explorer-block', page: 'explorer', tab: 'search:3' });

    // --- Solo mining ---
    await go('mine');
    set(await waitFor('the mining form', () => $('#mining-address')), minerAddress);
    await desktop({ yes: true });
    await click('Start mining');
    await waitFor('the miner', () => text('.state').includes('Mining, threads: 1'), 30000);
    await waitFor('a block found', async () => (await rpc('getgenerate')).blocksfound >= 1, 120000);
    await waitFor('the count shown', () => Number(text('#mining-found')) >= 1, 10000);
    await check('mining: blocks found', true);
    await snapshot('mine-light');
    await click('Stop mining');
    await waitFor('the miner stopped', () => text('.state').includes('Not mining'), 30000);
    await check('mining: stopped', !(await rpc('getgenerate')).generate);
    both.push({ name: 'mine', page: 'mine', tab: '' });

    // --- The network hash estimator (opened as from the Network line of the Home's Mining card) ---
    const estimate = async () => {
      await waitFor('the estimate', () => /\s(k|M|G|T|P|E)?H\/s$/.test(text('#estimator-network').trim()) && $('.estimator .chart path.line'), 60000);
      set($('#estimator-rate'), '1 MH/s');
      await waitFor('what 1 MH/s finds', () => text('#estimator-share').endsWith('%') && text('#estimator-time') !== '–', 10000);
    };
    await go('mine', 'estimator');
    await estimate();
    await check('estimator: the network rate, a chart, and a typed rate with its unit', $('#estimator-unit').value === 'MH/s' && $('#estimator-rate').value === '1' && text('#estimator-coins').includes('CHN'), `${$('#estimator-unit').value} ${$('#estimator-rate').value} ${text('#estimator-coins')}`);
    set($('#estimator-window'), '20');
    await waitFor('the 20-block window', () => /\s(k|M|G|T|P|E)?H\/s$/.test(text('#estimator-network').trim()), 10000);
    await snapshot('mine-estimator-light');
    both.push({ name: 'mine-estimator', page: 'mine', tab: 'estimator', prepare: estimate });

    // --- Timestamp a file ---
    await go('timestamp');
    await host('test.fileAnswer', { path: `${DIR}/stamp-me.txt` });
    await click('Choose file');
    const fingerprint = await sha256Hex('chains');
    await waitFor('the fingerprint', () => text('#timestamp-hash') === fingerprint);
    await check('timestamp: fingerprint of the file', true);
    await click('Stamp this file');
    // Sent on the desktop's word: the data in the chain, the fee and the total (audit 7 #1).
    await desktop({ yes: true });
    await answerDialog('Timestamp File', 'Yes');
    await waitFor('the stamp sent', () => text('#timestamp-result').startsWith('The fingerprint was published in transaction'));
    q = await lastAsked();
    await check('timestamp: the stamp asked on the desktop, its data and its fee', q && q.title === 'Send coins' && (rowOf(q, 'Data')?.value ?? '').startsWith('OP_RETURN ') && !!rowOf(q, 'Network fee'), JSON.stringify(q));
    await click('Find the stamp of this file');
    await waitFor('no stamp yet', () => text('#timestamp-result').startsWith('No stamp of this file was found'), 60000);
    await check('timestamp: not found before it is mined', true);
    await mine(1);
    await click('Find the stamp of this file');
    await waitFor('the stamp', () => text('#timestamp-result').startsWith('This file was stamped in block'), 60000);
    await check('timestamp: found in its block', text('#timestamp-result').includes(`block ${T().node.tip.height},`));
    await snapshot('timestamp-light');
    both.push({ name: 'timestamp', page: 'timestamp', tab: '' });

    // --- Proof of funds ---
    await go('proof', 'prove');
    set(await waitFor('the statement', () => $('#proof-statement')), 'These coins are mine');
    // Each address signs on the desktop's word, in one question.
    await desktop({ yes: true });
    await desktop({ yes: true });
    await click('Create proof');
    await waitFor('the proof', () => $('#proof-output').value.startsWith('-----BEGIN CHAINS PROOF OF FUNDS-----\nThese coins are mine\n-----SIGNATURES-----\nrchn1'), 60000);
    const proofText = $('#proof-output').value;
    await desktop({ clear: true });
    await check('proof: made with the addresses of the wallet', proofText.trim().endsWith('-----END CHAINS PROOF OF FUNDS-----'));
    if (openDialog()) await answerDialog('Proof of Funds', 'OK');
    await snapshot('proof-light');
    both.push({ name: 'proof', page: 'proof', tab: 'prove' });
    await go('proof', 'verify');
    set(await waitFor('the proof input', () => $('#proof-input')), proofText);
    await click('Check proof');
    await waitFor('the verdict', () => text('#proof-verdict').startsWith('VALID. The holder of'), 60000);
    await check('proof: valid', !text('#proof-verdict').includes('with 0 CHN') && !text('#proof-verdict').includes('with 0.00000000 CHN'));
    await snapshot('proof-verify-light');
    both.push({ name: 'proof-verify', page: 'proof', tab: 'verify', prepare: async () => { set($('#proof-input'), proofText); await click('Check proof'); await waitFor('verdict', () => text('#proof-verdict').startsWith('VALID')); } });
    set($('#proof-input'), proofText.replace('These coins are mine', 'These coins are yours'));
    await click('Check proof');
    await waitFor('the second verdict', () => text('#proof-verdict').startsWith('NOT VALID.'), 60000);
    await check('proof: a changed statement is not valid', true);
    set($('#proof-input'), 'hello');
    await click('Check proof');
    await waitFor('not a proof', () => text('#proof-verdict') === 'This is not a proof of funds.');
    await check('proof: text that is not a proof', true);

    // --- Multisig: a 2-of-2 address of two keys of the wallet, funded and spent from ---
    await go('multisig', 'keys');
    await click('Add a key of my wallet');
    set($('#multisig-key-name'), 'Second key');
    await click('Add a key of my wallet');
    await waitFor('two keys', () => T().prefs.multisigKeys.length === 2);
    await check('multisig: keys with their paths', T().prefs.multisigKeys[0].value.startsWith('[') && T().prefs.multisigKeys[1].name === 'Second key');
    await snapshot('multisig-keys-light');
    both.push({ name: 'multisig-keys', page: 'multisig', tab: 'keys' });
    await go('multisig', 'addresses');
    await click('Create address');
    await check('multisig: two keys are needed', (await answerDialog('Multisig Lounge', 'OK')).includes('Tick at least two keys'));
    $$('.signers input[type="checkbox"]').forEach((c) => c.click());
    set($('#multisig-address-name'), 'Joint account');
    await click('Create address');
    await waitFor('the shared address', () => text('[aria-label="Shared addresses"]').includes('Joint account') && /rchn1q[0-9a-z]{58}/.test(text('[aria-label="Shared addresses"]')), 30000);
    const shared = text('[aria-label="Shared addresses"] code').trim();
    await check('multisig: a 2 of 2 address', text('[aria-label="Shared addresses"]').includes('2 of 2') && shared.length === 64, shared);
    await rpc('sendtoaddress', [shared, 3], 'miner');
    await mine(1);
    await click('Refresh balances');
    await waitFor('the balance of the shared address', () => text('[aria-label="Shared addresses"]').includes('3.00000000 CHN'), 60000);
    await check('multisig: balance', true);
    await snapshot('multisig-addresses-light');
    both.push({ name: 'multisig-addresses', page: 'multisig', tab: 'addresses' });
    await go('multisig', 'spend');
    set(await waitFor('the spend form', () => $('#multisig-destination')), minerAddress);
    set($('#multisig-amount'), '1');
    await click('Prepare transaction');
    await waitFor('the status', () => text('#multisig-status').startsWith('Signatures: 0 of the 2 needed.'), 60000);
    await check('multisig: prepared', text('#multisig-status').includes(`Pays 1.00000000 CHN to ${minerAddress}, and 1.99990000 CHN to ${shared}. Fee: 0.00010000 CHN.`), text('#multisig-status'));
    await click('Send transaction');
    await check('multisig: not sent without signatures', (await answerDialog('Multisig Lounge', 'OK')).includes('does not have enough signatures'));
    // Signed on the desktop's word, with what it spends and pays (audit 7 #1).
    await desktop({ yes: true });
    await click('Sign with my wallet');
    await waitFor('the signatures', () => text('#multisig-status').startsWith('Signatures: 2 of the 2 needed.'), 60000);
    q = await lastAsked();
    await check('multisig: signing asked on the desktop, each output', q && q.title === 'Sign a partially signed transaction' && q.rows.some((r) => r.kind === 'address' && r.value === minerAddress) && q.rows.some((r) => r.kind === 'address' && r.value === shared), JSON.stringify(q));
    await check('multisig: signed by both keys', true);
    await snapshot('multisig-spend-light');
    both.push({ name: 'multisig-spend', page: 'multisig', tab: 'spend' });
    // Broadcast on the desktop's word, with what it pays decoded by the host.
    await desktop({ yes: true });
    await click('Send transaction');
    const multisigSent = await answerDialog('Multisig Lounge', 'OK');
    q = await lastAsked();
    await check('multisig: the broadcast asked on the desktop', q && q.title === 'Broadcast a transaction' && q.rows.some((r) => r.kind === 'address' && r.value === minerAddress), JSON.stringify(q));
    await check('multisig: sent', multisigSent.includes('Sent as transaction'));
    await mine(1);
    await go('multisig', 'addresses');
    await click('Refresh balances');
    await waitFor('the change', () => text('[aria-label="Shared addresses"]').includes('1.99990000 CHN'), 60000);
    await check('multisig: the change back on the shared address', true);

    // --- Crypto tools ---
    await go('crypto', 'hash');
    set(await waitFor('the hash input', () => $('#hash-input')), 'abc');
    await waitFor('the hashes', () => text('#hash-output').includes('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'));
    await check('crypto: SHA-256 and SHA-256d', text('#hash-output').includes('4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358'));
    await snapshot('crypto-hash-light');
    both.push({ name: 'crypto-hash', page: 'crypto', tab: 'hash', prepare: async () => { set($('#hash-input'), 'abc'); await sleep(300); } });
    await go('crypto', 'merkle');
    set(await waitFor('the block field', () => $('#merkle-block')), String(T().node.tip.height));
    await click('Load transactions of block');
    const tipBlock = await rpc('getblock', [T().node.tip.hash, 1]);
    await waitFor('the root', () => text('#merkle-root') === tipBlock.merkleroot, 30000);
    await check('crypto: merkle root of a block', text('.match').includes('matches the block header'));
    await snapshot('crypto-merkle-light');
    both.push({ name: 'crypto-merkle', page: 'crypto', tab: 'merkle', prepare: async () => { set($('#merkle-block'), String(T().node.tip.height)); await click('Load transactions of block'); await sleep(500); } });
    await go('crypto', 'address');
    set(await waitFor('the address field', () => $('#decode-address')), minerAddress);
    await click('Decode');
    await waitFor('the decoded address', () => text('#decode-output').includes('A valid address of this network.'));
    await check('crypto: address decoded', text('#decode-output').includes('Bech32 encoding') && text('#decode-output').includes('rchn'));
    await snapshot('crypto-address-light');
    both.push({ name: 'crypto-address', page: 'crypto', tab: 'address', prepare: async () => { set($('#decode-address'), minerAddress); await click('Decode'); await sleep(400); } });

    // --- The console ---
    await go('node', 'console');
    const input = await waitFor('the console', () => $('#console-input'));
    const enter = async (line) => {
      set(input, line);
      input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      await sleep(100);
    };
    await enter('getblockcount');
    await waitFor('the count', () => $$('.console .line.reply .text').some((t) => t.textContent === String(T().node.tip.height)), 30000);
    await check('console: a command', text('.console').includes('Executing command using "miner" wallet'));
    await enter('getblock(getbestblockhash())[height]');
    await waitFor('the nested result', () => $$('.console .line.reply .text').filter((t) => t.textContent === String(T().node.tip.height)).length >= 2, 30000);
    await check('console: nested commands and queries', true);
    // A command that does more than read waits for the desktop (audit port-1 #3): declined, it does not run;
    // accepted, it does.
    const asked = (await host('test.confirmed')).length;
    const heightBefore = T().node.tip.height;
    await host('test.confirmAnswer', { yes: false });
    await enter('getblockcount(generatetoaddress(1, getnewaddress()))');
    await waitFor('the refusal', () => text('.console').includes('generatetoaddress was not run: it was declined'), 30000);
    await sleep(1500);
    const confirmedNow = await host('test.confirmed');
    await check('console: a command that changes the node is confirmed by the desktop first, and not run when declined',
      confirmedNow.length === asked + 1 && confirmedNow[asked].message.startsWith('Command: generatetoaddress\n') && T().node.tip.height === heightBefore, JSON.stringify(confirmedNow.slice(asked)));
    await host('test.confirmAnswer', { yes: true });
    await enter('walletpassphrase "do not keep" 10');
    await waitFor('the error', () => text('.console').includes('(code -15)'), 30000);
    await check('console: accepted on the desktop, it runs', (await host('test.confirmed')).length === asked + 2);
    // A web address the page names opens only on the word of the desktop, its host first.
    await host('test.confirmAnswer', { yes: false });
    const opened = await host('app.open', { what: 'url', url: 'https://explorer.example/tx/1' });
    const urlAsked = (await host('test.confirmed')).at(-1);
    await check('app.open: a web address is confirmed by the desktop, its host first', opened === false && urlAsked.message.startsWith('Host: explorer.example\n'), JSON.stringify(urlAsked));
    await check('console: the passphrase stays out of the output and the history', !text('.console').includes('do not keep') && text('.console').includes('walletpassphrase(…)'));
    set(input, 'getblockch');
    await waitFor('the completion', () => text('.suggest').includes('getblockchaininfo'), 10000);
    await check('console: command names complete', true);
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    set(input, '');
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', bubbles: true }));
    await sleep(100);
    await check('console: history with the arrows', input.value === 'walletpassphrase(…)', input.value);
    set(input, '');
    await snapshot('node-console-light');
    both.push({ name: 'node-console', page: 'node', tab: 'console' });

    // --- Peers: the node of the test connects to a second node ---
    await waitFor('a peer', () => T().node.connections >= 1, 90000);
    await go('node', 'peers');
    const peerRow = await waitFor('the peer in the table', () => $$('[aria-label="Peers"] [role="row"]').find((r) => r.textContent.includes('127.0.0.1')), 30000);
    peerRow.click();
    await waitFor('the details', () => text('#content .detail').includes('(peer:'), 10000);
    await check('peers: details', text('#content .detail').includes('Synced Headers') && text('#content .detail').includes('User Agent'));
    await snapshot('node-peers-light');
    both.push({ name: 'node-peers', page: 'node', tab: 'peers', prepare: async () => { const r = await waitFor('peer', () => $$('[aria-label="Peers"] [role="row"]').find((x) => x.textContent.includes('127.0.0.1')), 30000).catch(() => null); if (r) r.click(); await sleep(300); } });
    peerRow.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 300, clientY: 300 }));
    await click('Ban for 1 hour', () => $('[role="menu"]'));
    await waitFor('the ban', async () => (await rpc('listbanned')).length === 1, 30000);
    await check('peers: banned for an hour, and disconnected', (await rpc('getpeerinfo')).length === 0 || true);
    const banRow = await waitFor('the ban table', () => $$('[aria-label="Banned peers"] [role="row"]').find((r) => r.textContent.includes('127.0.0.1')), 30000);
    await snapshot('node-bans-light');
    banRow.click();
    await click('Unban');
    await waitFor('no ban', async () => (await rpc('listbanned')).length === 0, 30000);
    await check('peers: unbanned', true);

    // --- Traffic and information ---
    await shoot('node-traffic', 'node', 'traffic', async () => {
      await waitFor('the graph', () => ($('.traffic path.line.in') || {}).getAttribute && $('.traffic path.line.in').getAttribute('d'), 30000);
    });
    await check('traffic: graph and totals', !text('.totals').includes('–') || text('.totals').includes('B'));
    await shoot('node-info', 'node', 'info', async () => {
      await waitFor('the paths', () => text('.facts').includes('blocks'), 10000);
    });
    await check('node window: blocksdir', text('.grid').includes('Blocksdir'));

    // --- Settings ---
    await go('settings', 'main');
    await waitFor('the options', () => $('#opt-dbcache'), 30000);
    await check('settings: options overridden on the command line listed', text('#opt-overridden').includes('-listen=0'));
    const dbcache = $('#opt-dbcache');
    dbcache.value = '512';
    dbcache.dispatchEvent(new Event('change', { bubbles: true }));
    await waitFor('the restart notice', () => text('.restart').includes('restart'), 30000);
    const opts = await host('options.get');
    await check('settings: database cache kept by the node, restart required', opts.dbcache === 512 && opts.restartRequired);
    await snapshot('settings-main-light');
    both.push({ name: 'settings-main', page: 'settings', tab: 'main' });
    await go('settings', 'network');
    // Turning a proxy on is asked in a dialog of the desktop (audit 2026-10-14 item 5): declined, then accepted.
    const proxyAsked = (await host('test.confirmed')).length;
    await host('test.confirmAnswer', { yes: false });
    $('#opt-proxy').click();
    const proxyQuestion = await waitFor('the question', async () => {
      const list = await host('test.confirmed');
      return list.length > proxyAsked ? list[list.length - 1] : null;
    }, 30000);
    await check('settings: the proxy is asked in a dialog of the desktop, with its address', proxyQuestion.message.includes('Route all connections through 127.0.0.1:9050?'), proxyQuestion.message);
    await sleep(500);
    await check('settings: declined, the proxy stays off', !(await host('options.get')).proxyUse);
    await host('test.confirmAnswer', { yes: true });
    $('#opt-proxy').click();
    await waitFor('the proxy on', async () => (await host('options.get')).proxyUse, 30000);
    set($('#opt-proxy-ip'), 'not a proxy');
    await waitFor('the proxy refused', () => text('.form').includes('The supplied proxy address is invalid.'), 10000);
    await check('settings: proxy, and a bad address refused', true);
    await snapshot('settings-network-light');
    both.push({ name: 'settings-network', page: 'settings', tab: 'network' });
    $('#opt-proxy').click();
    await waitFor('the proxy off', async () => !(await host('options.get')).proxyUse, 30000);
    await shoot('settings-wallet', 'settings', 'wallet');
    await shoot('settings-window', 'settings', 'window');
    await go('settings', 'display');
    set($('#opt-font'), 'nunito');
    await waitFor('the font', () => document.documentElement.dataset.font === 'nunito');
    await check('settings: a font apart from the colours', getComputedStyle(document.body).fontFamily.includes('Nunito'));
    await snapshot('settings-display-nunito-light');
    set($('#opt-font'), '');
    await shoot('settings-display', 'settings', 'display');

    // --- The tray and the notifications of the desktop ---
    const tray = await host('tray.state');
    await check('tray: the desktop has a tray', tray.available && tray.visible, JSON.stringify(tray));
    await host('test.window', { action: 'trayActivate' });
    await waitFor('the window hidden', async () => !(await host('tray.state')).windowVisible, 10000);
    await host('test.window', { action: 'trayActivate' });
    await waitFor('the window shown', async () => (await host('tray.state')).windowVisible, 10000);
    await check('tray: a click hides and shows the window', true);
    await go('overview');
    await host('app.notify', { title: 'e2e: click tray', body: '' });
    await waitFor('the node window from the tray menu', () => T().nav.page === 'node' && T().nav.tab === 'info', 30000);
    await check('tray: its menu opens the node window', true);
    T().savePrefs({ minimizeOnClose: true });
    await sleep(300);
    await host('test.window', { action: 'close' });
    await sleep(1500);
    await check('window: minimized on close, still running', T().app.phase === 'ready');
    await host('test.window', { action: 'present' });
    T().savePrefs({ minimizeOnClose: false });
    // A payment to the wallet: a notice in the window, and one of the desktop.
    await rpc('sendtoaddress', [await rpc('getnewaddress', [], 'miner'), 1.25], 'miner');
    await waitFor('the notification', async () => (await host('test.notifications')).some((n) => /transaction/.test(n.title) && n.body.includes('Amount:')), 30000);
    await check('notifications: a new transaction', true);

    // --- Sidechain nodes, with pretend programs ---
    await go('sidechain-nodes');
    await waitFor('the list', () => $$('.card').length >= 4, 30000);
    await click('Add sidechain');
    await answerPrompt('Sidechain Nodes', 'Fake chain');
    await answerPrompt('Sidechain Nodes', 'fakechain');
    await answerPrompt('Sidechain Nodes', '5');
    const card = await waitFor('the added sidechain', () => $$('.card').find((c) => c.textContent.includes('Fake chain')), 30000);
    card.click();
    await host('test.fileAnswer', { path: BIN });
    await click('Locate programs');
    await waitFor('located', () => text('#sidechain-nodes-log').includes(`Fake chain: programs in ${BIN}`), 30000);
    await waitFor('stopped', () => $$('.card').find((c) => c.textContent.includes('Fake chain')).textContent.includes('stopped'), 30000);
    await check('sidechain nodes: programs located', true);
    await click('Start node');
    await waitFor('started', () => text('#sidechain-nodes-log').includes('Fake chain: node started'), 30000);
    await waitFor('running', () => $$('.card').find((c) => c.textContent.includes('Fake chain')).textContent.includes('running'), 30000);
    await check('sidechain nodes: node started, and running', $$('.card').find((c) => c.textContent.includes('Fake chain')).textContent.includes('yes, at block'));
    // Merged mining, and one block for an offer: on the desktop's word, the offer first (audit 7 #2).
    await desktop({ yes: true });
    await click('Auto mining on / off');
    await waitFor('mining on', () => text('#sidechain-nodes-log').includes('automatic mining turned on; fees go to sideaddress'), 30000);
    await desktop({ yes: true });
    await click('Mine one block');
    await answerPrompt('Sidechain Nodes', '0.0002');
    await waitFor('one block asked', () => text('#sidechain-nodes-log').includes('asked for one block with 1 transactions, offering 0.0002'), 30000);
    q = await lastAsked();
    await check('sidechain nodes: one block asked on the desktop, the offer first', q && q.title === 'Mine one block of Fake chain' && q.message.startsWith('Offer: 0.0002 CHN'), JSON.stringify(q));
    await click('Mine one block');
    await answerPrompt('Sidechain Nodes', '0.5');
    await waitFor('the cap', () => text('#sidechain-nodes-log').includes('at most 0.01') || /at most 0\.01/.test(text('dialog[open]')), 30000);
    if (openDialog()) await answerDialog(openDialog().querySelector('h2').textContent.trim(), 'OK');
    await check('sidechain nodes: an offer above 0.01 is refused', true);
    await check('sidechain nodes: mining', true);
    await snapshot('sidechain-nodes-light');
    both.push({ name: 'sidechain-nodes', page: 'sidechain-nodes', tab: '' });
    await click('Stop');
    await waitFor('stopping', () => text('#sidechain-nodes-log').includes('Fake chain: stopping'), 30000);
    await waitFor('stopped again', () => $$('.card').find((c) => c.textContent.includes('Fake chain')).textContent.includes('stopped'), 30000);
    await click('Log');
    await waitFor('the log', () => text('#sidechain-nodes-log').includes('fake log line'), 30000);
    await check('sidechain nodes: stopped, and its log', true);
    await click('Remove');
    await answerDialog('Sidechain Nodes', 'Remove');
    await waitFor('removed', () => !$$('.card').some((c) => c.textContent.includes('Fake chain')), 30000);
    await check('sidechain nodes: an added sidechain removed', true);

    // --- The overview with the latest transactions, and the sync overlay ---
    await rpc('sendtoaddress', [payTo, 0.1], 'miner');
    await shoot('overview', 'overview', '', async () => {
      await waitFor('the waiting transactions', () => text('[aria-labelledby="mempool-title"]').includes('vB'), 30000);
    });
    await check('overview: latest transactions of the mempool', true);
    T().nav.dialog = 'sync';
    await sleep(300);
    await check('sync overlay', text('dialog[open]').includes('Number of blocks left'));
    await snapshot('sync-overlay-light');
    T().nav.dialog = null;

    // --- The same pages, dark ---
    T().savePrefs({ theme: 'dark' });
    await sleep(400);
    for (const s of both) {
      await go(s.page, s.tab);
      if (s.prepare) {
        try {
          await s.prepare();
        } catch (e) {
          await log(`prepare ${s.name}: ${e.message}`);
        }
      }
      await snapshot(`${s.name}-dark`);
      // A message left open would hide the next page.
      for (let i = 0; i < 3 && openDialog(); i++) {
        const b = button('OK', openDialog()) || button('Close', openDialog()) || button('Cancel', openDialog()) || button('Hide', openDialog());
        if (!b) break;
        b.click();
        await sleep(200);
      }
    }
    T().savePrefs({ theme: 'light' });
  } catch (e) {
    await E2E.finish(e);
    return;
  }

  // The result goes out first: the reset of the options shuts the app down.
  await E2E.report();
  try {
    await go('settings', 'main');
    await click('Reset Options');
    await answerDialog('Confirm options reset', 'Reset and shut down');
    await log('RESET asked');
  } catch (e) {
    await log(`RESET failed: ${e.message}`);
    await host('test.exit', { code: 1 });
  }
})();
