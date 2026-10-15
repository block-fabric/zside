// A hardware wallet (audit: parity item G06), with the mock signer of the functional tests
// (test/functional/mocks/signer.py, as wallet_signer.py uses it): a wallet made with it, an address shown on
// the device, and a payment signed on the device from the send page.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const $$ = (s, root = document) => [...root.querySelectorAll(s)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const TPRV = "tprv8ZgxMBicQKsPd7Uf69XL1XwhmjHopUGep8GuEiJDZmbQz6o58LninorQAfcKZWARbtRtfnLcJ5MQ2AtHcQJCCRUcMRvmDUjyEmNUWwx8UbK";
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    T().nav.dialog = null;
    const signers = await host('wallets.signers');
    await check('G06 the signer of -signer is found', signers.supported && signers.signers.length === 1, JSON.stringify(signers));
    const made = await host('wallets.create', { name: 'hww', disablePrivateKeys: true, externalSigner: true });
    await check('G06 a wallet of the hardware wallet', made.name === 'hww');
    const address1 = await rpc('getnewaddress', ['', 'bech32'], 'hww');
    await check('G06 its addresses come from the device', address1 === 'rchn1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th6atc9d9', address1);
    await host('wallet.displayAddress', { wallet: 'hww', address: address1 });
    await check('G06 an address shown on the device', true);
    await rpc('getnewaddress', ['', 'p2sh-segwit'], 'hww');
    await rpc('getnewaddress', ['', 'legacy'], 'hww');
    const address4 = await rpc('getnewaddress', ['', 'bech32m'], 'hww');

    // Coins for it, and the transaction the device will answer with (made by a wallet with its keys).
    await rpc('createwallet', ['miner']);
    const minerAddress = await rpc('getnewaddress', [], 'miner');
    await rpc('generatetoaddress', [101, minerAddress]);
    await rpc('sendtoaddress', [address4, 1], 'miner');
    await rpc('generatetoaddress', [1, minerAddress]);
    await rpc('createwallet', ['mock', false, true]);
    await rpc('importdescriptors', [[
      { desc: `tr([00000001/86h/1h/0']${TPRV}/0/*)#7ew68cn8`, timestamp: 0, range: [0, 1], internal: false, active: true },
      { desc: `tr([00000001/86h/1h/0']${TPRV}/1/*)#0dtm6drl`, timestamp: 0, range: [0, 0], internal: true, active: true },
    ]], 'mock');
    const dest = await rpc('getnewaddress', ['', 'bech32'], 'miner');
    const funded = await rpc('walletcreatefundedpsbt', [[], { [dest]: 0.5 }, 0, { replaceable: true }, true], 'mock');
    const signed = await rpc('walletprocesspsbt', [funded.psbt, true, 'ALL', true], 'mock');
    const mockTx = await rpc('decoderawtransaction', [signed.hex]);
    // The device answers with it: the mock reads mock_psbt where it runs.
    await host('test.fileAnswer', { path: window.__SIGNER_DIR + '/mock_psbt' });
    const path = await host('file.choose', { save: true, title: 'mock' });
    await host('file.write', { path, text: signed.psbt });

    // The send page, as the user: "Sign on device".
    T().selectWallet('hww');
    await waitFor('the balance of hww', () => T().wallets.balances.hww && T().wallets.balances.hww.trusted > 0n, 30000);
    T().go('send');
    await sleep(600);
    set(document.getElementById(document.querySelector('[id^="pay-to-"]').id), dest);
    await sleep(500);
    set(document.querySelector('[id^="amount-"]'), '0.5');
    await sleep(400);
    const sendButton = await waitFor('Sign on device', () => $$('button').find((b) => b.textContent.includes('Sign on device') && !b.disabled), 20000);
    await check('G06 the send button signs on the device', sendButton.title === 'Connect your hardware wallet first.', sendButton.title);
    sendButton.click();
    const confirm = await waitFor('the confirmation', () => $$('dialog[open]').find((d) => d.textContent.includes('Confirm send coins')), 30000);
    const sign = await waitFor('its button', () => $$('button', confirm).find((b) => b.textContent.trim().startsWith('Sign on device') && !b.disabled), 10000);
    sign.click();
    await waitFor('sent', () => T().nav.highlightTx, 30000);
    await check('G06 the device signed, and the transaction is sent', T().nav.highlightTx === mockTx.txid, `${T().nav.highlightTx} / ${mockTx.txid}`);
    const mempool = await rpc('getrawmempool');
    await check('G06 it is in the mempool', mempool.includes(mockTx.txid));
    await snapshot('audit-signer-send-light');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
