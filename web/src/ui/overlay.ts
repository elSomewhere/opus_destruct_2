/**
 * Full-screen states and notifications: loading progress, "click to play", fatal errors
 * (no WebGPU, engine not built, device lost) and transient toasts.
 */
import { h } from './dom.ts';

export class Overlay {
  private readonly parent: HTMLElement;
  private readonly loading: HTMLElement;
  private readonly loadingText: HTMLElement;
  private readonly loadingBar: HTMLElement;
  private readonly prompt: HTMLElement;
  private readonly toasts: HTMLElement;

  constructor(parent: HTMLElement) {
    this.parent = parent;
    this.loadingText = h('div', { class: 'loading-text' }, 'Starting');
    this.loadingBar = h('div', { class: 'loading-bar-fill' });
    this.loading = h('div', { class: 'loading' }, this.loadingText, h('div', { class: 'loading-bar' }, this.loadingBar));
    this.prompt = h('div', { class: 'prompt hidden' }, 'Click to play');
    this.toasts = h('div', { class: 'toasts' });
    parent.append(this.loading, this.prompt, this.toasts);
  }

  setLoading(text: string | null, fraction = 0): void {
    this.loading.classList.toggle('hidden', text === null);
    if (text === null) return;
    this.loadingText.textContent = text;
    this.loadingBar.style.width = `${Math.round(Math.max(0, Math.min(1, fraction)) * 100)}%`;
  }

  setPrompt(visible: boolean): void {
    this.prompt.classList.toggle('hidden', !visible);
  }

  toast(message: string, kind: 'info' | 'error' = 'info', ms = 6000): void {
    const t = h('div', { class: `toast ${kind}` }, message);
    this.toasts.append(t);
    setTimeout(() => t.remove(), ms);
    while (this.toasts.childElementCount > 5) this.toasts.firstElementChild?.remove();
  }

  fatal(title: string, detail: string, links: { href: string; label: string }[] = []): void {
    this.setLoading(null);
    this.setPrompt(false);
    this.parent.append(
      h(
        'div',
        { class: 'fatal' },
        h('h1', {}, title),
        h('p', {}, detail),
        ...links.map((l) => h('p', {}, h('a', { href: l.href }, l.label))),
      ),
    );
  }
}
