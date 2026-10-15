// What the console of the node window holds while the app runs: its lines and its history.
import { ConsoleHistory, type ConsoleKind, type ConsoleLine } from './console';

const MAX_LINES = 2000;
let nextId = 1;

export const consoleState = $state({
  lines: [] as ConsoleLine[],
  executing: false,
  /** The wallet the last command ran on ('' none): the console says when it changes. */
  lastWallet: null as string | null,
  commands: [] as string[],
  welcomed: false,
});

export const history = new ConsoleHistory();

export function say(kind: ConsoleKind, text: string): number {
  const id = nextId++;
  consoleState.lines.push({ id, kind, time: Date.now(), text });
  if (consoleState.lines.length > MAX_LINES) consoleState.lines.splice(0, consoleState.lines.length - MAX_LINES);
  return id;
}

export function replace(id: number, kind: ConsoleKind, text: string): void {
  const line = consoleState.lines.find((l) => l.id === id);
  if (line) {
    line.kind = kind;
    line.text = text;
  } else {
    say(kind, text);
  }
}

export function clearConsole(): void {
  consoleState.lines = [];
  consoleState.welcomed = false;
}
