// The chain's own tabs of Settings (chain/LocalSettings.svelte shows them): none in the template. BitAssets and Layers
// add "Asset filters" (the assets a wallet hides, per wallet). Each: its id (go('settings', '<id>')) and its label.
export const LOCAL_SETTINGS_TABS: readonly { id: string; label: string }[] = [];
