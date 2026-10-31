// Integration test driver for OBS BLE Sensors (dev-mode build, simulated sensor).
// Usage: node obs_itest.mjs <phase>   phase = "first" | "restart"
import zlib from 'node:zlib';

const URL = 'ws://127.0.0.1:4466';
const SIM_KEY = 'ble:public:DE:00:00:00:00:01';
const UNSUPPORTED_KEY = 'ble:public:DE:00:00:00:00:02';
const phase = process.argv[2] || 'first';

function decodePng(buf) {
  let pos = 8, width = 0, height = 0, colorType = 0, bitDepth = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('ascii', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9];
    } else if (type === 'IDAT') idat.push(data);
    pos += 12 + len;
  }
  if (bitDepth !== 8 || (colorType !== 6 && colorType !== 2)) throw new Error(`unsupported png ${bitDepth}/${colorType}`);
  const bpp = colorType === 6 ? 4 : 3;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * bpp;
  const out = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const f = raw[y * (stride + 1)];
    for (let x = 0; x < stride; x++) {
      const v = raw[y * (stride + 1) + 1 + x];
      const a = x >= bpp ? out[y * stride + x - bpp] : 0;
      const b = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= bpp && y > 0 ? out[(y - 1) * stride + x - bpp] : 0;
      let r;
      switch (f) {
        case 0: r = v; break;
        case 1: r = v + a; break;
        case 2: r = v + b; break;
        case 3: r = v + ((a + b) >> 1); break;
        case 4: { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
          r = v + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c); break; }
        default: throw new Error('bad filter');
      }
      out[y * stride + x] = r & 0xff;
    }
  }
  let visible = 0;
  for (let i = 0; i < width * height; i++) if (bpp === 4 ? out[i * 4 + 3] > 0 : (out[i * 3] | out[i * 3 + 1] | out[i * 3 + 2])) visible++;
  return { width, height, visible };
}

class Obs {
  constructor() { this.id = 0; this.pending = new Map(); }
  async connect() {
    for (let i = 0; i < 60; i++) {
      try { await this.tryConnect(); return; } catch { await sleep(1000); }
    }
    throw new Error('could not connect to obs-websocket');
  }
  tryConnect() {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(URL, 'obswebsocket.json');
      ws.onerror = () => reject(new Error('ws error'));
      ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.op === 0) ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1, eventSubscriptions: 0 } }));
        else if (msg.op === 2) { this.ws = ws; resolve(); }
        else if (msg.op === 7) {
          const p = this.pending.get(msg.d.requestId);
          if (p) { this.pending.delete(msg.d.requestId); p(msg.d); }
        }
      };
    });
  }
  call(requestType, requestData = {}) {
    const requestId = String(++this.id);
    return new Promise((resolve) => {
      this.pending.set(requestId, resolve);
      this.ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
    });
  }
  async ok(requestType, requestData) {
    const r = await this.call(requestType, requestData);
    if (!r.requestStatus.result) throw new Error(`${requestType} failed: ${JSON.stringify(r.requestStatus)}`);
    return r.responseData;
  }
  async shot(sourceName) {
    const r = await this.call('GetSourceScreenshot', { sourceName, imageFormat: 'png' });
    if (!r.requestStatus.result) return { error: r.requestStatus.comment || r.requestStatus.code };
    const b64 = r.responseData.imageData.split(',')[1];
    return decodePng(Buffer.from(b64, 'base64'));
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const log = (...a) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1).padStart(5)}s]`, ...a);

const obs = new Obs();
await obs.connect();
for (let i = 0; i < 60; i++) {
  const r = await obs.call('GetCurrentProgramScene');
  if (r.requestStatus.result) break;
  await sleep(500);
}
const ver = await obs.ok('GetVersion');
log(`OBS ${ver.obsVersion}, websocket ${ver.obsWebSocketVersion}, platform ${ver.platformDescription}`);

const kinds = (await obs.ok('GetInputKindList', { unversioned: false })).inputKinds;
log('ble_heart_rate_source registered:', kinds.includes('ble_heart_rate_source'));
const scene = (await obs.ok('GetCurrentProgramScene')).currentProgramSceneName;

async function sample(names, seconds) {
  const rows = [];
  for (let i = 0; i < seconds; i++) {
    const parts = [];
    for (const n of names) {
      const s = await obs.shot(n);
      parts.push(s.error ? `${n}=ERR(${s.error})` : `${n}=${s.width}x${s.height} visiblePx=${s.visible}`);
      rows.push({ n, ...s, t: (Date.now() - t0) / 1000 });
    }
    log(parts.join('  '));
    await sleep(1000);
  }
  return rows;
}

if (phase === 'first') {
  await obs.ok('SetVideoSettings', { fpsNumerator: 60, fpsDenominator: 1, baseWidth: 1920, baseHeight: 1080, outputWidth: 1920, outputHeight: 1080 });
  const vs = await obs.ok('GetVideoSettings');
  log(`video ${vs.baseWidth}x${vs.baseHeight} @ ${vs.fpsNumerator}/${vs.fpsDenominator}`);

  log('A: source without a sensor');
  await obs.ok('CreateInput', { sceneName: scene, inputName: 'HR-none', inputKind: 'ble_heart_rate_source', inputSettings: {}, sceneItemEnabled: true });
  await sample(['HR-none'], 3);

  log('C/D/E/F: source on simulated sensor (40 s cycle: 20 s data, 10 s silent, 5 s off, 5 s back)');
  await obs.ok('CreateInput', { sceneName: scene, inputName: 'HR', inputKind: 'ble_heart_rate_source', inputSettings: { device_key: SIM_KEY }, sceneItemEnabled: true });
  log('L: second source on the same sensor');
  await obs.ok('CreateInput', { sceneName: scene, inputName: 'HR2', inputKind: 'ble_heart_rate_source', inputSettings: { device_key: SIM_KEY, display_style: 0 }, sceneItemEnabled: true });
  const statsBefore = await obs.ok('GetStats');
  await sample(['HR', 'HR2'], 48);
  const statsAfter = await obs.ok('GetStats');
  log(`stats: fps=${statsAfter.activeFps.toFixed(1)} avgRender=${statsAfter.averageFrameRenderTime.toFixed(3)}ms ` +
      `renderSkipped=${statsAfter.renderSkippedFrames - statsBefore.renderSkippedFrames}/${statsAfter.renderTotalFrames - statsBefore.renderTotalFrames} cpu=${statsAfter.cpuUsage.toFixed(1)}%`);

  log('K/G: switch HR2 to the unsupported simulated device');
  await obs.ok('SetInputSettings', { inputName: 'HR2', inputSettings: { device_key: UNSUPPORTED_KEY } });
  await sample(['HR2'], 4);
  log('G: switch HR2 back to the heart-rate sensor');
  await obs.ok('SetInputSettings', { inputName: 'HR2', inputSettings: { device_key: SIM_KEY } });
  await sample(['HR2'], 3);

  // Move HR so the transform can be checked after restart.
  const { sceneItemId } = await obs.ok('GetSceneItemId', { sceneName: scene, sourceName: 'HR' });
  await obs.ok('SetSceneItemTransform', { sceneName: scene, sceneItemId, sceneItemTransform: { positionX: 321, positionY: 123 } });
  log('done (phase first)');
} else if (phase === 'release') {
  log('release DLL + real WinRT backend: source pointed at an absent sensor');
  await obs.ok('CreateInput', { sceneName: scene, inputName: 'HR-real', inputKind: 'ble_heart_rate_source', inputSettings: { device_key: 'ble:public:11:22:33:44:55:66' }, sceneItemEnabled: true });
  await sample(['HR-real'], 20);
  const st = await obs.ok('GetStats');
  log(`stats: fps=${st.activeFps.toFixed(1)} renderSkipped=${st.renderSkippedFrames}/${st.renderTotalFrames}`);
  log('done (phase release)');
} else {
  log('B/J: after restart');
  const s = await obs.ok('GetInputSettings', { inputName: 'HR' });
  log('HR settings:', JSON.stringify(s.inputSettings));
  const { sceneItemId } = await obs.ok('GetSceneItemId', { sceneName: scene, sourceName: 'HR' });
  const tr = (await obs.ok('GetSceneItemTransform', { sceneName: scene, sceneItemId })).sceneItemTransform;
  log(`HR transform position ${tr.positionX},${tr.positionY}`);
  await sample(['HR', 'HR-none'], 25);
  log('done (phase restart)');
}
process.exit(0);
