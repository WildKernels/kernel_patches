var TCPC = (() => {
  var __defProp = Object.defineProperty;
  var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
  var __getOwnPropNames = Object.getOwnPropertyNames;
  var __hasOwnProp = Object.prototype.hasOwnProperty;
  var __export = (target, all) => {
    for (var name in all)
      __defProp(target, name, { get: all[name], enumerable: true });
  };
  var __copyProps = (to, from, except, desc) => {
    if (from && typeof from === "object" || typeof from === "function") {
      for (let key of __getOwnPropNames(from))
        if (!__hasOwnProp.call(to, key) && key !== except)
          __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
    }
    return to;
  };
  var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

  // src/index.js
  var index_exports = {};
  __export(index_exports, {
    apply: () => apply,
    getStatus: () => getStatus
  });

  // node_modules/kernelsu/index.js
  var callbackCounter = 0;
  function getUniqueCallbackName(prefix) {
    return `${prefix}_callback_${Date.now()}_${callbackCounter++}`;
  }
  function exec(command, options) {
    if (typeof options === "undefined") {
      options = {};
    }
    return new Promise((resolve, reject) => {
      const callbackFuncName = getUniqueCallbackName("exec");
      window[callbackFuncName] = (errno, stdout, stderr) => {
        resolve({ errno, stdout, stderr });
        cleanup(callbackFuncName);
      };
      function cleanup(successName) {
        delete window[successName];
      }
      try {
        ksu.exec(command, JSON.stringify(options), callbackFuncName);
      } catch (error) {
        reject(error);
        cleanup(callbackFuncName);
      }
    });
  }
  function Stdio() {
    this.listeners = {};
  }
  Stdio.prototype.on = function(event, listener) {
    if (!this.listeners[event]) {
      this.listeners[event] = [];
    }
    this.listeners[event].push(listener);
  };
  Stdio.prototype.emit = function(event, ...args) {
    if (this.listeners[event]) {
      this.listeners[event].forEach((listener) => listener(...args));
    }
  };
  function ChildProcess() {
    this.listeners = {};
    this.stdin = new Stdio();
    this.stdout = new Stdio();
    this.stderr = new Stdio();
  }
  ChildProcess.prototype.on = function(event, listener) {
    if (!this.listeners[event]) {
      this.listeners[event] = [];
    }
    this.listeners[event].push(listener);
  };
  ChildProcess.prototype.emit = function(event, ...args) {
    if (this.listeners[event]) {
      this.listeners[event].forEach((listener) => listener(...args));
    }
  };

  // src/index.js
  var MODROOT = "/data/adb/modules/tcp-config";
  var USE_KSU = typeof ksu !== "undefined";
  async function getStatus() {
    if (USE_KSU) {
      const { stdout } = await exec(
        `sysctl -n net.ipv4.tcp_congestion_control; sysctl -n net.core.default_qdisc; zcat /proc/config.gz 2>/dev/null | grep -oE '^CONFIG_DEFAULT_TCP_CONG="[^"]*"' | cut -d"'" -f2; cat /data/adb/tcpcfg.state 2>/dev/null`
      );
      const lines = stdout.trim().split("\n");
      const st = { algo: lines[0] || "", qdisc: lines[1] || "", dflt: lines[2] || "unknown", state_algo: "", state_qdisc: "" };
      for (let i = 3; i < lines.length; i++) {
        const kv = lines[i].split("=");
        if (kv[0] === "ALGO") st.state_algo = kv[1] || "";
        if (kv[0] === "QDISC") st.state_qdisc = kv[1] || "";
      }
      return st;
    }
    const r = await fetch("status");
    return r.json();
  }
  async function apply(algo, qdisc) {
    if (USE_KSU) {
      const { errno, stdout } = await exec(`sh ${MODROOT}/webroot/apply.sh ${algo} ${qdisc}`);
      if (errno !== 0) return { ok: false, error: "exec failed: " + errno };
      try {
        return JSON.parse(stdout);
      } catch {
        return { ok: false, error: stdout };
      }
    }
    const r = await fetch("set?algo=" + algo + "&qdisc=" + qdisc);
    return r.json();
  }
  return __toCommonJS(index_exports);
})();
