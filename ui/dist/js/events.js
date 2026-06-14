import { api } from "./api.js";

// Live state via SSE, falling back to polling /api/state. Returns a stop fn.
export function connect(onState) {
  let stopped = false;
  let polling = false;
  let timer = null;
  let source = null;

  const poll = async () => {
    if (stopped) return;
    try {
      onState(await api.getState());
    } catch {
      // keep last state
    }
    timer = setTimeout(poll, 1000);
  };

  const startPolling = () => {
    if (polling) return;
    polling = true;
    poll();
  };

  try {
    source = new EventSource("/api/events");
    source.onmessage = e => onState(JSON.parse(e.data));
    source.onerror = () => {
      source.close();
      startPolling();
    };
  } catch {
    startPolling();
  }

  return () => {
    stopped = true;
    clearTimeout(timer);
    source?.close?.();
  };
}
