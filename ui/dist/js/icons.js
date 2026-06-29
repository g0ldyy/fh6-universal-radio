const STROKE = 'stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"';
const svg = inner => `<svg viewBox="0 0 24 24" width="24" height="24" fill="none" aria-hidden="true">${inner}</svg>`;

export const icons = {
  broadcast: svg(
    `<path ${STROKE} d="M4.9 16.2a10 10 0 0 1 0-8.4M19.1 7.8a10 10 0 0 1 0 8.4M7.9 13.4a5 5 0 0 1 0-2.8M16.1 10.6a5 5 0 0 1 0 2.8"/><circle cx="12" cy="12" r="1.8" fill="currentColor"/>`,
  ),
  gear: svg(
    `<path ${STROKE} d="M12.2 2h-.4a2 2 0 0 0-2 2v.2a2 2 0 0 1-1 1.7l-.4.3a2 2 0 0 1-2 0l-.2-.1a2 2 0 0 0-2.7.7l-.2.4a2 2 0 0 0 .7 2.7l.2.1a2 2 0 0 1 1 1.7v.5a2 2 0 0 1-1 1.7l-.2.1a2 2 0 0 0-.7 2.7l.2.4a2 2 0 0 0 2.7.7l.2-.1a2 2 0 0 1 2 0l.4.3a2 2 0 0 1 1 1.7v.2a2 2 0 0 0 2 2h.4a2 2 0 0 0 2-2v-.2a2 2 0 0 1 1-1.7l.4-.3a2 2 0 0 1 2 0l.2.1a2 2 0 0 0 2.7-.7l.2-.4a2 2 0 0 0-.7-2.7l-.2-.1a2 2 0 0 1-1-1.7v-.5a2 2 0 0 1 1-1.7l.2-.1a2 2 0 0 0 .7-2.7l-.2-.4a2 2 0 0 0-2.7-.7l-.2.1a2 2 0 0 1-2 0l-.4-.3a2 2 0 0 1-1-1.7V4a2 2 0 0 0-2-2Z"/><circle cx="12" cy="12" r="3" ${STROKE}/>`,
  ),
  close: svg(`<path ${STROKE} d="M18 6 6 18M6 6l12 12"/>`),
  prev: svg(`<path fill="currentColor" d="M7 6h2v12H7z"/><path fill="currentColor" d="M19 6 10 12l9 6z"/>`),
  next: svg(`<path fill="currentColor" d="M15 6h2v12h-2z"/><path fill="currentColor" d="M5 6l9 6-9 6z"/>`),
  play: svg(`<path fill="currentColor" d="M8 5v14l11-7z"/>`),
  pause: svg(`<path fill="currentColor" d="M8 5h3v14H8zM13 5h3v14h-3z"/>`),
  shuffle: svg(
    `<path ${STROKE} d="M2 18h1.4c1.3 0 2.5-.6 3.3-1.7l6.1-8.6c.7-1.1 2-1.7 3.3-1.7H22"/><path ${STROKE} d="m18 2 4 4-4 4"/><path ${STROKE} d="M2 6h1.9c1.5 0 2.9.9 3.6 2.2"/><path ${STROKE} d="M22 18h-5.9c-1.3 0-2.6-.7-3.3-1.8l-.5-.8"/><path ${STROKE} d="m18 14 4 4-4 4"/>`,
  ),
  undo: svg(`<path ${STROKE} d="M3 12a9 9 0 1 0 3-6.7"/><path ${STROKE} d="M3 3v5h5"/>`),
};
