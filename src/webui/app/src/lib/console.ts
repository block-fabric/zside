// The console of the node window (RPCConsole of chains-qt): the history of commands, the
// completion of command names, and the messages it shows.

/** Commands kept in the history (CONSOLE_HISTORY). */
export const CONSOLE_HISTORY = 50;

export type ConsoleKind = 'request' | 'reply' | 'error' | 'info';

export interface ConsoleLine {
  id: number;
  kind: ConsoleKind;
  time: number;
  text: string;
}

/**
 * The history of the console: the lines entered (already filtered of sensitive arguments), the
 * newest last, each at most once, at most CONSOLE_HISTORY; and the place of the user browsing it.
 */
export class ConsoleHistory {
  lines: string[] = [];
  private pointer = 0;

  constructor(saved: string[] = []) {
    for (const line of saved) this.add(line);
  }

  /** Add an entered line (RPCConsole::on_lineEdit_returnPressed). */
  add(line: string): void {
    const i = this.lines.indexOf(line);
    if (i >= 0) this.lines.splice(i, 1);
    this.lines.push(line);
    while (this.lines.length > CONSOLE_HISTORY) this.lines.shift();
    this.pointer = this.lines.length;
  }

  /** Move in the history (up: -1, down: +1); the line to show (empty past the newest). */
  browse(offset: number): string {
    this.pointer = Math.max(0, Math.min(this.lines.length, this.pointer + offset));
    return this.pointer < this.lines.length ? this.lines[this.pointer] : '';
  }

  /** Back to the end, as after a new line. */
  reset(): void {
    this.pointer = this.lines.length;
  }
}

/** The names of the RPC commands in the text of "help" (lines that start with a command). */
export function commandsFromHelp(help: string): string[] {
  const names = new Set<string>();
  for (const line of help.split('\n')) {
    const m = /^([a-z][a-z0-9_]*)\b/.exec(line);
    if (m) names.add(m[1]);
  }
  names.add('help-console');
  return [...names].sort();
}

/** Command names that complete what is typed: the last word, when it is the start of a command. */
export function completions(input: string, commands: readonly string[]): string[] {
  const m = /([a-z][a-z0-9_-]*)$/i.exec(input);
  if (!m) return [];
  // Only where a command goes: at the start, or nested after "(". Arguments are not completed.
  const before = input.slice(0, input.length - m[1].length);
  if (!(before.trim() === '' || /\(\s*$/.test(before))) return [];
  const prefix = m[1].toLowerCase();
  return commands.filter((c) => c.startsWith(prefix) && c !== prefix).slice(0, 12);
}

/** The input with its last word replaced by a completion. */
export function complete(input: string, command: string): string {
  return input.replace(/([a-z][a-z0-9_-]*)$/i, command);
}

/** The welcome message of the console, with the warning about scammers (RPCConsole::clear). */
export function welcome(clientName: string): string {
  return (
    `Welcome to the ${clientName} RPC console.\n` +
    'Use up and down arrows to navigate history, and Ctrl+L to clear screen.\n' +
    'Use Ctrl++ and Ctrl+- to increase or decrease the font size.\n' +
    'Type help for an overview of available commands.\n' +
    'For more information on using this console, type help-console.'
  );
}

export const SCAM_WARNING =
  'WARNING: Scammers have been active, telling users to type commands here, stealing their wallet contents. Do not use this console without fully understanding the ramifications of a command.';
