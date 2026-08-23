// tcp-config WebUI logic — dual channel:
//   1. KernelSU WebUI: kernelsu.exec() runs apply.sh / status commands
//   2. Browser fallback: fetch() against the nc server on :8090
// Built with esbuild; the output goes to webroot/index.js.
import { exec } from 'kernelsu';

const MODROOT = '/data/adb/modules/tcp-config';
const USE_KSU = typeof ksu !== 'undefined';

export async function getStatus() {
  if (USE_KSU) {
    const { stdout } = await exec(
      'sysctl -n net.ipv4.tcp_congestion_control; sysctl -n net.core.default_qdisc; ' +
      'zcat /proc/config.gz 2>/dev/null | grep -oE \'^CONFIG_DEFAULT_TCP_CONG="[^"]*"\' | cut -d"\'" -f2; ' +
      'cat /data/adb/tcpcfg.state 2>/dev/null'
    );
    const lines = stdout.trim().split('\n');
    const st = { algo: lines[0] || '', qdisc: lines[1] || '', dflt: lines[2] || 'unknown', state_algo: '', state_qdisc: '' };
    for (let i = 3; i < lines.length; i++) {
      const kv = lines[i].split('=');
      if (kv[0] === 'ALGO') st.state_algo = kv[1] || '';
      if (kv[0] === 'QDISC') st.state_qdisc = kv[1] || '';
    }
    return st;
  }
  const r = await fetch('status');
  return r.json();
}

export async function apply(algo, qdisc) {
  if (USE_KSU) {
    const { errno, stdout } = await exec(`sh ${MODROOT}/webroot/apply.sh ${algo} ${qdisc}`);
    if (errno !== 0) return { ok: false, error: 'exec failed: ' + errno };
    try { return JSON.parse(stdout); } catch { return { ok: false, error: stdout }; }
  }
  const r = await fetch('set?algo=' + algo + '&qdisc=' + qdisc);
  return r.json();
}
