// Streaming-download adapter based on OpenBLOPS-NX's installer (GPL-3.0).
export async function downloadWritable(name) {
  const registration = await navigator.serviceWorker.ready;
  const worker = registration.active;
  if (!worker) throw new Error('Reload the page to prepare the ZIP download helper.');
  const channel = new MessageChannel(), port = channel.port1;
  let credits = 0, cancelled = false, wake;
  const ready = new Promise(resolve => {
    port.onmessage = ({ data }) => {
      if (data.type === 'ready') resolve();
      else if (data.type === 'pull') ++credits;
      else if (data.type === 'cancel') cancelled = true;
      if (wake) { const callback = wake; wake = null; callback(); }
    };
  });
  const id = crypto.randomUUID();
  worker.postMessage({ type: 'download', id, name }, [channel.port2]);
  await ready;
  const keepAlive = setInterval(() => worker.postMessage({ type: 'ping' }), 10000);
  const frame = document.createElement('iframe');
  frame.hidden = true;
  frame.src = `./download/${id}/${encodeURIComponent(name)}`;
  document.body.append(frame);
  const capacity = 1 << 20;
  const buffer = new Uint8Array(capacity);
  let used = 0;
  async function flush() {
    if (!used) return;
    while (!credits && !cancelled) await new Promise(resolve => { wake = resolve; });
    if (cancelled) throw new DOMException('The download was cancelled.', 'AbortError');
    --credits;
    const chunk = buffer.slice(0, used);
    port.postMessage({ type: 'chunk', chunk: chunk.buffer }, [chunk.buffer]);
    used = 0;
  }
  function cleanup() {
    clearInterval(keepAlive);
    setTimeout(() => { frame.remove(); port.close(); }, 60000);
  }
  return {
    async write(bytes) {
      if (cancelled) throw new DOMException('The download was cancelled.', 'AbortError');
      for (let at = 0; at < bytes.length;) {
        const count = Math.min(capacity - used, bytes.length - at);
        buffer.set(bytes.subarray(at, at + count), used);
        at += count; used += count;
        if (used === capacity) await flush();
      }
    },
    async close() { try { await flush(); port.postMessage({ type: 'close' }); } finally { cleanup(); } },
    async abort() { port.postMessage({ type: 'error', message: 'Installation stopped.' }); cleanup(); },
  };
}
