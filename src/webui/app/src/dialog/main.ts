// The page of the dialogs of the host (dialog.html): see dialog.ts.
import '../styles/fonts.css';
import '../styles/themes.css';
import './dialog.css';
import { start, type DialogBridge } from './dialog';

declare global {
  interface Window {
    __chainsDialog?: DialogBridge;
  }
}

const bridge = window.__chainsDialog;
if (typeof bridge === 'function') {
  // Nothing shown on an error: the host shows the dialog of the system instead when the page says nothing in time.
  start((method, params) => bridge(method, params)).catch(() => undefined);
}
