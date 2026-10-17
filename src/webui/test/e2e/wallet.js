// The wallet pages of chains-app on regtest, driven through the page as a user would: create
// wallets (one encrypted), receive, mine, send with the passphrase, the history and its details,
// a fee bump, a PSBT made, saved, loaded, signed and broadcast, encryption, unlocking for a while,
// a new passphrase, messages signed and verified, the receiving addresses (a tab of Receive),
// backups, choosing the coins in the Send page, and
// pictures of every wallet page and dialog.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const DIR = window.__E2E_FILES;
  const PASS = 'correct horse battery staple';

  // --- Driving the interface ---
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(60);
  }
  /** The dialog a page waits for, with a title. */
  async function dialogTitled(title, ms = 20000) {
    return waitFor(`the dialog "${title}"`, () => {
      const d = openDialog();
      return d && $('h2', d) && $('h2', d).textContent.trim() === title ? d : null;
    }, ms);
  }
  /** Answer the dialog shown with a button of its own. */
  async function answerDialog(title, label) {
    const d = await dialogTitled(title);
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function passphrase(title, fields) {
    const d = await dialogTitled(title);
    for (const [id, value] of Object.entries(fields)) set($(`#${id}`, d), value);
    await sleep(50);
    $('#passphrase-form').requestSubmit();
    await waitFor('the passphrase dialog to go', () => !$('#passphrase-form'));
  }
  const walletOf = (name) => T().wallets.list.find((w) => w.name === name);
  // The questions of the desktop (the dialog window of the host): what pays, signs or unlocks is asked there, with the
  // passphrase typed there, never in the page (audit 7 #1). The test answers the next one, and reads what it asked.
  const desktopAnswers = (answer) => host('test.confirmAnswer', answer);
  const lastAsked = async () => (await host('test.confirmed')).at(-1);
  const rowOf = (asked, label) => (asked && asked.rows ? asked.rows.find((r) => r.label === label) : undefined);
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(150);
  }

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    T().savePrefs({ theme: 'light' });
    await go('wallets');
    await snapshot('wallets-empty-light');

    // --- Create an encrypted wallet (CreateWalletDialog, then AskPassphraseDialog) ---
    await go('wallets', 'create');
    let d = await dialogTitled('Create Wallet');
    set($('#wallet-name', d), 'alice');
    $$('input[type="checkbox"]', d)[0].click();
    await snapshot('wallet-create-light');
    await click('Create', d);
    d = await dialogTitled('Encrypt wallet');
    set($('#pass2', d), PASS);
    set($('#pass3', d), PASS + 'x');
    await sleep(50);
    $('#passphrase-form').requestSubmit();
    await waitFor('the mismatch', () => text('dialog[open]').includes('The supplied passphrases do not match.'));
    await check('passphrases that differ are refused', true);
    set($('#pass3', d), PASS);
    await snapshot('wallet-passphrase-light');
    $('#passphrase-form').requestSubmit();
    await answerDialog('Confirm wallet encryption', 'Continue');
    await answerDialog('Wallet to be encrypted', 'Yes');
    await waitFor('alice', () => walletOf('alice') && walletOf('alice').encrypted && walletOf('alice').locked, 60000);
    await check('create an encrypted wallet', T().wallets.current === 'alice');

    // A second wallet, without encryption.
    await go('wallets', 'create');
    d = await dialogTitled('Create Wallet');
    set($('#wallet-name', d), 'bob');
    await click('Create', d);
    await waitFor('bob', () => walletOf('bob') && T().wallets.current === 'bob', 60000);
    await check('create a second wallet', !walletOf('bob').encrypted);
    T().selectWallet('alice');

    // --- Receive: a request with a new address, its QR code, its history ---
    await go('receive');
    set(await waitFor('the receive form', () => $('#req-label')), 'Mining');
    set($('#req-message'), 'Block rewards');
    set($('#req-amount'), '12.5');
    await sleep(150);
    await click('Create new receiving address');
    d = await dialogTitled('Request payment to Mining');
    const uriText = $('.uri', d).textContent;
    const minerAddress = uriText.slice('chains:'.length, uriText.indexOf('?'));
    await check('request: URI with amount, label and message', uriText.startsWith('chains:rchn1') && uriText.includes('amount=12.5') && uriText.includes('label=Mining') && uriText.includes('message=Block%20rewards'), uriText);
    await check('request: QR code drawn', !!$('svg.qr', d));
    await snapshot('receive-request-light');
    await host('test.fileAnswer', { path: `${DIR}/request.png` });
    await click('Save image', d);
    await waitFor('the picture saved', () => text('.toasts').includes('request.png'));
    await check('request: QR picture saved', true);
    await click('Close', d);
    await waitFor('the request in the history', () => text('.history').includes('Block rewards'));
    await check('request kept in the history', true);
    const requests = await host('wallet.requests', { wallet: 'alice' });
    await check('request stored in the wallet as chains-qt stores it', requests.length === 1 && requests[0].amount === 1250000000 && requests[0].label === 'Mining', JSON.stringify(requests));

    // --- Mine to it ---
    await rpc('generatetoaddress', [101, minerAddress]);
    await waitFor('the balance', () => T().wallets.balances.alice && T().wallets.balances.alice.trusted === 5000000000n, 60000);
    await check('mined coins in the balance', true);
    await go('overview');
    await waitFor('recent transactions', () => $$('.recent .tx').length >= 5, 30000);
    await check('overview: recent transactions', text('.recent').includes('Mining'));
    await check('overview: balance chart', !!$('.balances .chart svg path.line'));
    await snapshot('overview-wallet-light');

    // --- Send with the passphrase, through the confirmation ---
    const bobAddress = await rpc('getnewaddress', ['from alice'], 'bob');
    await go('send');
    set(await waitFor('the send form', () => $('input[id^="pay-to-"]')), bobAddress);
    set($('input[id^="label-"]'), 'Bob');
    set($('input[id^="amount-"]'), '10');
    await waitFor('a valid address', () => text('.send').includes('Valid address'));
    await waitFor('the smart fee', () => /CHN\/kvB/.test(text('.fee')));
    await snapshot('send-filled-light');
    await click('Send', () => $('.actions'));
    // The page's preview: no passphrase asked in the page.
    d = await dialogTitled('Confirm send coins');
    await check('send: no passphrase in the page', !$('#passphrase-form'));
    await check('send: confirmation with fee and total', /Transaction fee \([\d.]+ kvB\)/.test(d.textContent) && d.textContent.includes('Total Amount: 10.0'), d.textContent.slice(0, 300));
    await check('send: the address first, then the amount, then the label', d.textContent.includes(`To ${bobAddress}: 10.00000000 CHN`) && d.textContent.indexOf(bobAddress) < d.textContent.indexOf('(label \u201cBob\u201d)'), d.textContent.slice(0, 400));
    const sendButton = $('button[data-choice="send"]', d);
    await check('send: the button waits', sendButton.disabled && /\(\d\)/.test(sendButton.textContent));
    await snapshot('send-confirm-light');
    // Declined on the desktop: nothing is sent.
    await desktopAnswers({ yes: false });
    await click('Send', d);
    await waitFor('declined', () => text('.toasts').includes('Nothing was sent'), 30000);
    let asked = await lastAsked();
    await check('send: the desktop asked, with what the transaction pays read from it',
      asked && asked.title === 'Send coins' && asked.passphrase && rowOf(asked, 'Pay to')?.value === bobAddress && rowOf(asked, 'Amount')?.value === '10.00000000 CHN' &&
        rowOf(asked, 'Label')?.value === '\u201cBob\u201d' && /CHN$/.test(rowOf(asked, 'Network fee')?.value ?? '') && /^10\.0000\d+ CHN$/.test(rowOf(asked, 'Total')?.value ?? '') &&
        asked.rows.findIndex((r) => r.label === 'Pay to') < asked.rows.findIndex((r) => r.label === 'Amount'),
      JSON.stringify(asked));
    await check('send: declined, nothing sent', (await rpc('getrawmempool', [])).length === 0);
    // Again, accepted on the desktop with the passphrase typed there.
    await click('Send', () => $('.actions'));
    d = await dialogTitled('Confirm send coins');
    await desktopAnswers({ yes: true, passphrase: PASS });
    await click('Send', d);
    await waitFor('the transaction sent', () => T().nav.highlightTx, 30000);
    const sentTxid = T().nav.highlightTx;
    await waitFor('alice locked again', async () => (await rpc('getwalletinfo', [], 'alice')).unlocked_until === 0, 20000);
    await check('send: alice locked again', true);
    await waitFor('bob sees it', () => T().wallets.balances.bob && T().wallets.balances.bob.untrustedPending === 1000000000n, 30000);
    await check('send: bob receives 10', true);
    const book = await host('wallet.addresses', { wallet: 'alice' });
    await check('send: recipient in the address book with its label', book.some((e) => e.address === bobAddress && e.label === 'Bob' && e.purpose === 'send'));

    // --- The history: the row, its details, its menu, the fee bump ---
    await go('transactions');
    await waitFor('the sent transaction selected', () => $('.transactions [role="row"][aria-selected="true"]'), 30000);
    await check('transactions: the sent one selected', text('.transactions [aria-selected="true"]').includes('Bob'));
    await snapshot('transactions-wallet-light');
    const row = $('.transactions [role="row"][aria-selected="true"]');
    row.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 400, clientY: 300 }));
    await waitFor('the menu', () => $('.context-menu'));
    await waitFor('the bump action', () => {
      const b = button('Increase transaction fee', $('.context-menu'));
      return b && !b.disabled;
    });
    await snapshot('transactions-menu-light');
    await click('Show transaction details', $('.context-menu'));
    d = await dialogTitled('Transaction details');
    await waitFor('the details', () => d.textContent.includes('Transaction ID') && d.textContent.includes(sentTxid));
    await check('details: status, debit, fee', /0\/unconfirmed, in memory pool/.test(d.textContent) && d.textContent.includes('Transaction fee'), d.textContent.slice(0, 300));
    await snapshot('transaction-details-light');
    await click('Close', d);
    $('.transactions [role="row"][aria-selected="true"]').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 400, clientY: 300 }));
    await waitFor('the menu again', () => button('Increase transaction fee', $('.context-menu')) && !button('Increase transaction fee', $('.context-menu')).disabled);
    await click('Increase transaction fee', $('.context-menu'));
    d = await dialogTitled('Confirm fee bump');
    await check('bump: current, increase, new fee', d.textContent.includes('Current fee:') && d.textContent.includes('New fee:'));
    await snapshot('bump-fee-light');
    await desktopAnswers({ yes: true, passphrase: PASS });
    await click('Yes', d);
    const replaced = await waitFor('the replacement', async () => (await rpc('gettransaction', [sentTxid], 'alice')).replaced_by_txid, 30000);
    asked = await lastAsked();
    await check('bump: asked on the desktop, the transaction it replaces and the fees', asked && asked.title === 'Increase the fee' && rowOf(asked, 'Replaces')?.value === sentTxid && !!rowOf(asked, 'Fee before'), JSON.stringify(asked));
    await check('bump: the transaction replaced by one with a higher fee', /^[0-9a-f]{64}$/.test(replaced), String(replaced));

    // Export the history.
    await host('test.fileAnswer', { path: `${DIR}/transactions.csv` });
    await click('Export', () => $('.transactions .page-head'));
    await waitFor('the export', () => text('.toasts').includes('transactions.csv'));
    await check('transactions exported as CSV', true);

    // --- A PSBT: created unsigned, saved, loaded from the file, signed, broadcast ---
    await rpc('generatetoaddress', [1, minerAddress]);
    await waitFor('bob confirmed', () => T().wallets.balances.bob && T().wallets.balances.bob.trusted === 1000000000n, 30000);
    T().savePrefs({ psbtControls: true });
    T().selectWallet('bob');
    const aliceAddress = await rpc('getnewaddress', [], 'alice');
    await go('overview');
    await go('send');
    set(await waitFor('the send form', () => $('input[id^="pay-to-"]')), aliceAddress);
    set($('input[id^="amount-"]'), '2');
    await waitFor('a valid address', () => text('.send').includes('Valid address'));
    await click('Send', () => $('.actions'));
    d = await dialogTitled('Confirm send coins');
    await click('Create Unsigned', d);
    d = await dialogTitled('Unsigned Transaction');
    const psbtText = $('pre', d).textContent;
    await check('PSBT: shown and copied', psbtText.startsWith('cHNidP8'));
    await snapshot('psbt-created-light');
    await host('test.fileAnswer', { path: `${DIR}/payment.psbt` });
    await click('Save', d);
    await waitFor('the PSBT saved', () => text('.toasts').includes('PSBT saved'));
    await host('test.fileAnswer', { path: `${DIR}/payment.psbt` });
    await go('psbt', 'file');
    await waitFor('the PSBT loaded', () => text('.ops').includes('Sends'), 30000);
    await check('PSBT: loaded from the file, needs signatures', text('.ops').includes('Transaction still needs signature(s).') && text('.ops').includes('2.00000000'));
    await snapshot('psbt-loaded-light');
    await desktopAnswers({ yes: true });
    await click('Sign Tx');
    await waitFor('signed', () => text('.ops').includes('Signed transaction successfully'), 30000);
    asked = await lastAsked();
    await check('PSBT: signing asked on the desktop, what it spends of the wallet and pays', asked && asked.title === 'Sign a partially signed transaction' && rowOf(asked, 'Pay to')?.value === aliceAddress && !!rowOf(asked, 'Spends of this wallet'), JSON.stringify(asked));
    await desktopAnswers({ yes: true });
    await click('Broadcast Tx');
    await waitFor('broadcast', () => text('.ops').includes('Transaction broadcast successfully'), 30000);
    asked = await lastAsked();
    await check('PSBT: signed and broadcast, the broadcast asked on the desktop', asked && asked.title === 'Broadcast a transaction', JSON.stringify(asked));
    await snapshot('psbt-broadcast-light');
    T().savePrefs({ psbtControls: false });

    // --- Encrypt bob; unlock for a while; lock; change the passphrase ---
    await go('wallets', 'encrypt');
    await passphrase('Encrypt wallet', { pass2: 'bob pass', pass3: 'bob pass' });
    await answerDialog('Confirm wallet encryption', 'Continue');
    await answerDialog('Wallet encrypted', 'OK');
    await waitFor('bob encrypted', () => walletOf('bob').encrypted && walletOf('bob').locked, 30000);
    await check('encrypt an existing wallet', true);
    await go('wallets', 'unlock');
    d = await dialogTitled('Unlock wallet for a while');
    set($('#unlock-minutes', d), '1');
    await desktopAnswers({ yes: true, passphrase: 'bob pass' });
    await click('Unlock', d);
    await waitFor('bob unlocked', () => !walletOf('bob').locked, 30000);
    asked = await lastAsked();
    await check('unlock: the passphrase typed on the desktop, not in the page', asked && asked.passphrase && rowOf(asked, 'For')?.value === '1 minute' && !$('#passphrase-form'), JSON.stringify(asked));
    await check('unlock with a timeout', (await rpc('getwalletinfo', [], 'bob')).unlocked_until > 0);
    await snapshot('wallets-unlocked-light');
    await click('Lock now');
    await waitFor('bob locked', () => walletOf('bob').locked, 30000);
    await check('lock again', true);
    await go('wallets', 'passphrase');
    await passphrase('Change passphrase', { pass1: 'bob pass', pass2: 'bob pass 2', pass3: 'bob pass 2' });
    await answerDialog('Wallet encrypted', 'OK');
    await rpc('walletpassphrase', ['bob pass 2', 5], 'bob');
    await rpc('walletlock', [], 'bob');
    await check('change the passphrase', true);

    // --- Sign and verify a message ---
    T().selectWallet('alice');
    const signer = await rpc('getnewaddress', ['signing', 'bech32'], 'alice');
    await go('messages', `sign:${signer}`);
    set(await waitFor('the message box', () => $('#sign-message')), 'I own this address.');
    await sleep(100);
    await desktopAnswers({ yes: true, passphrase: PASS });
    await click('Sign Message', () => $('.messages form'));
    await waitFor('the signature', () => $('#signature-out').value.length > 40, 30000);
    asked = await lastAsked();
    await check('sign: asked on the desktop, the address and the message', asked && rowOf(asked, 'Address')?.value === signer && rowOf(asked, 'Message')?.value === 'I own this address.', JSON.stringify(asked));
    const signature = $('#signature-out').value;
    await check('sign a message with a P2WPKH address', text('.messages').includes('Message signed.'));
    await snapshot('messages-sign-light');
    await go('messages', 'verify');
    set(await waitFor('the verify form', () => $('#verify-address')), signer);
    set($('#verify-message'), 'I own this address.');
    set($('#verify-signature'), signature);
    await click('Verify Message', () => $('.messages form'));
    await waitFor('verified', () => text('.messages').includes('Message verified.'));
    set($('#verify-message'), 'I own this address');
    await click('Verify Message', () => $('.messages form'));
    await waitFor('refused', () => text('.messages').includes('Message verification failed.'));
    await check('verify a message, and a changed one fails', true);
    await snapshot('messages-verify-light');

    // --- The receiving addresses: a tab of Receive (no Addresses page, no sending addresses) ---
    await check('no sending addresses in the menus', !T().buildCommands().some((c) => /sending address/i.test(c.label)));
    T().buildCommands().find((c) => c.label === 'Receiving addresses').run();
    await sleep(150);
    await check('receiving addresses open the tab of Receive', T().nav.page === 'receive' && T().nav.tab === 'addresses', `${T().nav.page}/${T().nav.tab}`);
    const grid = await waitFor('receiving addresses', () => {
      const g = $('[role="grid"][aria-label="Receiving addresses"]');
      return g && g.textContent.includes('Mining') ? g : null;
    }, 30000);
    await check('receiving addresses: labels, used, received', grid.textContent.includes('signing') && grid.textContent.includes('Used') && grid.textContent.includes('Unused'), grid.textContent.slice(0, 300));
    set($('#addr-new-label'), 'Savings');
    set($('#addr-new-type'), 'bech32m');
    // A locked wallet asks for its passphrase, on the desktop, only when it has no address ready.
    await desktopAnswers({ yes: true, passphrase: PASS });
    await click('Generate new address');
    await waitFor('the new address', () => grid.textContent.includes('Savings') && grid.textContent.includes('Bech32m'), 30000);
    await desktopAnswers({ clear: true });
    const book2 = await host('wallet.addresses', { wallet: 'alice' });
    const savings = book2.find((e) => e.label === 'Savings');
    await check('receiving addresses: a new taproot address with its label', !!savings && savings.purpose === 'receive' && savings.address.startsWith('rchn1p'), JSON.stringify(savings));
    await click('Copy', () => $('.addresses .toolbar'));
    await waitFor('copied', () => text('.toasts').includes('Address'));
    await click('Edit label', () => $('.addresses .toolbar'));
    d = await dialogTitled('Edit receiving address');
    set($('#edit-label', d), 'Rainy day');
    await click('OK', d);
    await waitFor('the new label', () => grid.textContent.includes('Rainy day'), 20000);
    await check('receiving addresses: copy and a new label', true);
    await snapshot('receive-addresses-light');
    await click('QR code', () => $('.addresses .toolbar'));
    d = await waitFor('the QR code', () => { const x = openDialog(); return x && $('svg.qr', x) ? x : null; });
    await snapshot('receive-addresses-qr-light');
    await click('Close', d);
    await host('test.fileAnswer', { path: `${DIR}/receiving.csv` });
    await click('Export', () => $('.addresses .toolbar'));
    await waitFor('exported', () => text('.toasts').includes('receiving.csv'));

    // --- Backup, watch-only export ---
    await host('test.fileAnswer', { path: `${DIR}/alice-backup.dat` });
    await go('wallets', 'backup');
    await answerDialog('Backup Successful', 'OK');
    await host('test.fileAnswer', { path: `${DIR}/alice-watchonly.dat` });
    await go('wallets', 'export');
    await answerDialog('Export Successful', 'OK');
    await check('backup and watch-only export', true);

    // --- Restore the backup under another name, close it, open it again ---
    await host('test.fileAnswer', { path: `${DIR}/alice-backup.dat` });
    await go('wallets', 'restore');
    d = await dialogTitled('Restore Wallet');
    set($('#prompt-input', d), 'alice-restored');
    await click('OK', d);
    await answerDialog('Restore wallet message', 'OK');
    await waitFor('the restored wallet', () => walletOf('alice-restored'), 60000);
    await check('restore a backup', walletOf('alice-restored').encrypted);
    await go('wallets', 'close');
    await answerDialog('Close wallet', 'Yes');
    await waitFor('closed', () => !walletOf('alice-restored'), 30000);
    await go('wallets');
    await waitFor('the list', () => button('Open', $('#wallet-dir')));
    await click('Open', $('#wallet-dir'));
    await waitFor('opened again', () => walletOf('alice-restored'), 60000);
    await check('close and open a wallet', true);
    T().selectWallet('alice');

    // --- Choose coins: the coins of the wallet in the Send page ---
    T().savePrefs({ coinControl: false });
    await go('send');
    await check('choose coins: off, the wallet chooses', text('.coins').includes('Coins are chosen automatically'));
    $('.coins input[role="switch"]').click();
    await sleep(100);
    const coinGrid = await waitFor('the coins', () => {
      const g = $('[role="grid"][aria-label="Coins of the wallet"]');
      return g && $$('input[type="checkbox"]', g).length > 1 ? g : null;
    }, 20000);
    await check('choose coins: in the page, no dialog', T().prefs.coinControl && !openDialog());
    set($('input[id^="pay-to-"]'), bobAddress);
    set($('input[id^="amount-"]'), '1000');
    $$('input[type="checkbox"]', coinGrid)[0].click();
    await waitFor('not enough', () => text('.coins [role="alert"]').includes('Not enough'), 20000);
    await check('choose coins: one coin is not enough for 1000', true);
    await snapshot('send-coins-short-light');
    set($('input[id^="amount-"]'), '1');
    await waitFor('the estimate', () => text('.coins').includes('Quantity') && !$('.coins [role="alert"]'), 20000);
    await check('choose coins: the coins chosen and their estimate', true);
    // Lock a coin that is not chosen, then unlock it.
    const lockable = $$('[role="row"]', coinGrid).find((r) => $('button[aria-label="Lock coin"]', r) && !$('input[type="checkbox"]', r).checked);
    if (lockable) {
      $('button[aria-label="Lock coin"]', lockable).click();
      await waitFor('locked', () => $('button[aria-label="Unlock coin"]', coinGrid), 20000);
      const n = (await rpc('listlockunspent', [], 'alice')).length;
      await check('choose coins: lock a coin', n === 1, String(n));
      $('button[aria-label="Unlock coin"]', coinGrid).click();
      await waitFor('unlocked', () => !$('button[aria-label="Unlock coin"]', coinGrid), 20000);
    }
    await snapshot('send-coins-light');
    T().savePrefs({ theme: 'dark' });
    await sleep(300);
    await snapshot('send-coins-dark');
    T().savePrefs({ theme: 'light' });
    // Spend only the coin chosen.
    const chosenRow = $$('[role="row"]', coinGrid).find((r) => { const b = $('input[type="checkbox"]', r); return b && b.checked; });
    const chosenAmount = chosenRow ? chosenRow.querySelectorAll('[role="gridcell"]')[1].textContent.trim() : '';
    await click('Send', () => $('.actions'));
    d = await dialogTitled('Confirm send coins');
    await click('Cancel', d);
    await check('choose coins: the send takes the coin chosen', !!chosenAmount, chosenAmount);
    $('.coins input[role="switch"]').click();
    await sleep(100);
    await check('choose coins: off again', !$('[role="grid"][aria-label="Coins of the wallet"]'));
    await click('Clear all');

    // --- A proof of funds of chosen coins (user decision, 2026-10-15), checked by another wallet ---
    await go('proof');
    await waitFor('the proof page', () => $('#proof-statement'), 20000);
    $('.choose input[role="switch"]').click();
    await click('By coin');
    const proofGrid = await waitFor('the coins to prove', () => {
      const g = $('[role="grid"][aria-label="Coins to prove"]');
      return g && $$('input[type="checkbox"]', g).length > 1 ? g : null;
    }, 20000);
    $$('input[type="checkbox"]', proofGrid)[0].click();
    $$('input[type="checkbox"]', proofGrid)[1].click();
    await waitFor('the total of the coins', () => text('.plan').includes('2 coin(s)'), 10000);
    await snapshot('proof-coins-light');
    // Signed on the desktop's word, the passphrase typed there: one question for every address.
    await desktopAnswers({ yes: true, passphrase: PASS });
    await click('Create proof');
    const proofText = await waitFor('the proof', () => ($('#proof-output').value.includes('-----END') ? $('#proof-output').value : null), 30000);
    asked = await lastAsked();
    await check('proof of funds: one question of the desktop, the message and each address', asked && asked.title === 'Sign a message' && asked.rows.filter((r) => r.label === 'Address').length >= 1, JSON.stringify(asked));
    await desktopAnswers({ clear: true });
    const listedCoins = proofText.split('-----COINS-----\n')[1]?.split('-----SIGNATURES-----')[0].trim().split('\n') ?? [];
    await check('proof of funds: the two coins chosen are listed', listedCoins.length === 2 && listedCoins.every((l) => /^[0-9a-f]{64}:\d+ \d+\.\d{8}$/.test(l)), listedCoins.join(' | '));
    T().selectWallet('bob');
    await waitFor('bob', () => T().wallets.current === 'bob');
    await go('proof', 'verify');
    const proofInput = await waitFor('the check of a proof', () => $('#proof-input'), 20000);
    set(proofInput, proofText);
    await click('Check proof');
    const verdictText = await waitFor('the verdict', () => (text('#proof-verdict').includes('Coins listed') ? text('#proof-verdict') : null), 60000);
    await check('proof of funds: checked by another wallet, both coins unspent on signed addresses', verdictText.startsWith('VALID') && verdictText.includes('Coins listed in the proof: 2. Unspent now, on addresses that signed: 2') && $('#proof-verdict').classList.contains('valid'), verdictText.slice(0, 300));
    await snapshot('proof-verify-coins-light');
    T().selectWallet('alice');
    await waitFor('alice', () => T().wallets.current === 'alice');

    // --- Pictures of the wallet pages, light and dark ---
    for (const theme of ['light', 'dark']) {
      T().savePrefs({ theme });
      for (const [page, tab] of [['overview', ''], ['send', ''], ['receive', ''], ['transactions', ''], ['receive', 'addresses'], ['messages', 'sign'], ['messages', 'verify'], ['psbt', ''], ['wallets', ''], ['settings', 'wallet']]) {
        await go(page, tab);
        await sleep(500);
        await snapshot(`w-${page}${tab ? '-' + tab : ''}-${theme}`);
      }
      // Dialogs.
      await go('wallets', 'create');
      await dialogTitled('Create Wallet');
      await snapshot(`w-create-dialog-${theme}`);
      $('dialog[open] button[aria-label="Close"]').click();
      await go('receive');
      const shown = await waitFor('a request row', () => $('.history [role="row"][aria-rowindex="2"]'));
      shown.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
      await dialogTitled('Request payment to Mining');
      await snapshot(`w-request-dialog-${theme}`);
      await click('Close', openDialog());
      T().savePrefs({ mask: true });
      await go('overview');
      await snapshot(`w-overview-masked-${theme}`);
      await go('transactions');
      await snapshot(`w-transactions-masked-${theme}`);
      T().savePrefs({ mask: false });
    }
    T().savePrefs({ theme: 'light' });
    await go('overview');
    await finish();
  } catch (e) {
    await log(`error: ${e && e.message}`).catch(() => undefined);
    await snapshot('wallet-failure').catch(() => undefined);
    await finish(e);
  }
})();
