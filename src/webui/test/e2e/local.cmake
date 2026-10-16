# The page scripts of zSide's own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# zside.js: the shielded wallet, memos, private activity, melt & cast, the pool.
webui_local_e2e(zside 29760)
# zside_simple.js: Simple mode (as a user chooses it): the private balance, Make private and Send
# privately through the pages, with pictures in Midnight and Light (the other scripts run in Advanced).
webui_local_e2e(zside_simple 41810 --js-var SIMPLE=true)
