// After a start again: the change of the options never applied is gone, the applied one stays (audit M7).
(async () => {
  const { host, T, waitFor, check, finish } = E2E;
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    const options = await host('options.get');
    await check('M7 a change never applied is taken back at the next start', options.prune === false && options.pending === 0, JSON.stringify({ prune: options.prune, pending: options.pending }));
    await check('M7 an applied change stays', options.par === 2, String(options.par));
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
