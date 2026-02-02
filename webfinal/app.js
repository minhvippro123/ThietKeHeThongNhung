// app.js — ESP32 HTTP control (NO fetch) + Firebase HMI/Chart read
// FIX: Send HTTP commands via hidden iframe queue (works even when fetch is blocked by browser)
// FIX: Chart live update from STATE (PATH_NODE) instead of /history (history listener removed)

const $ = (id) => document.getElementById(id);

const NODE_ID = "NODE_01";

// ESP32 firmware updates NODE directly: /nodes/NODE_01
const PATH_NODE    = `/nodes/${NODE_ID}`;
// history optional (NOT used for chart now)
const PATH_HISTORY = `/nodes/${NODE_ID}/history`;

let espIp = (localStorage.getItem("esp_ip") || "192.168.100.7").trim();

// ---------------- UI helpers ----------------
function setPill(id, text, ok=true){
  const el = $(id);
  if (!el) return;
  el.textContent = text;
  el.style.opacity = ok ? "1" : "0.7";
}
function setLog(msg){
  const el = $("log");
  if (el) el.textContent = String(msg ?? "—");
}
function normalizeIp(ip){
  ip = (ip || "").trim();
  ip = ip.replace(/^https?:\/\//i, "");
  ip = ip.replace(/\/+$/g, "");
  return ip;
}
function apiBase(){
  return `http://${espIp}`;
}

// ---------------- RGB preview ----------------
function syncPreview(){
  const r = $("r"), g = $("g"), b = $("b"), bri = $("bri");
  if (!r || !g || !b || !bri) return;

  const rv = +r.value, gv = +g.value, bv = +b.value, br = +bri.value;
  $("rv").textContent = rv;
  $("gv").textContent = gv;
  $("bv").textContent = bv;
  $("briv").textContent = br;

  const hex = "#" + [rv,gv,bv].map(x=>x.toString(16).padStart(2,"0")).join("");
  $("previewHex").textContent = hex;
  $("swatch").style.background = hex;
}

// ---------------- HTTP sender via hidden iframe queue ----------------
let _cmdFrame = null;
let _cmdBusy  = false;
let _cmdQ     = [];
let _cmdTimer = null;

function _ensureCmdFrame(){
  if (_cmdFrame) return _cmdFrame;
  const f = document.createElement("iframe");
  f.style.width = "0";
  f.style.height = "0";
  f.style.border = "0";
  f.style.position = "absolute";
  f.style.left = "-9999px";
  f.style.top  = "-9999px";
  document.body.appendChild(f);
  _cmdFrame = f;
  return f;
}

function _dequeueNext(){
  if (_cmdBusy) return;
  const job = _cmdQ.shift();
  if (!job) return;

  const { path, label } = job;
  const url0 = apiBase() + path;
  const url  = url0 + (url0.includes("?") ? "&" : "?") + "_ts=" + Date.now();

  _cmdBusy = true;
  setPill("pillConn", "SENDING…", true);
  setLog(`SEND: ${url}`);

  const f = _ensureCmdFrame();

  let done = false;
  const finish = (ok, reason) => {
    if (done) return;
    done = true;
    _cmdBusy = false;
    if (_cmdTimer) { clearTimeout(_cmdTimer); _cmdTimer = null; }

    if (ok){
      setPill("pillConn", "SENT", true);
      setLog(`SENT: ${label || path}`);
    }else{
      setPill("pillConn", "ESP OFFLINE", false);
      setLog(`FAIL: ${label || path}\n${reason || ""}`);
    }

    setTimeout(_dequeueNext, 60);
  };

  f.onload = () => finish(true, "onload");
  f.onerror = () => finish(false, "onerror");
  _cmdTimer = setTimeout(() => finish(false, "timeout"), 1500);

  try{
    f.src = url;
  }catch(e){
    finish(false, e?.message || String(e));
  }
}

function apiSend(path, label){
  _cmdQ.push({ path, label });
  _dequeueNext();
}

// ---------------- Controls ----------------
function bindControls(){
  const ipInput = $("ip");
  if (ipInput) ipInput.value = espIp;

  $("btnSaveIp")?.addEventListener("click", () => {
    espIp = normalizeIp(ipInput?.value || espIp) || espIp;
    localStorage.setItem("esp_ip", espIp);
    setPill("pillConn", "IP saved", true);
    setLog(`IP SAVED: ${espIp}`);
  });

  $("btnPing")?.addEventListener("click", ()=>{
    apiSend(`/api/health`, "/api/health");
  });

  $("btnOn")?.addEventListener("click", ()=>{
    apiSend(`/api/power?on=1`, "POWER ON");
  });

  $("btnOff")?.addEventListener("click", ()=>{
    apiSend(`/api/power?on=0`, "POWER OFF");
  });

  document.querySelectorAll(".segbtn").forEach(btn=>{
    btn.addEventListener("click", ()=>{
      document.querySelectorAll(".segbtn").forEach(b=>b.classList.remove("active"));
      btn.classList.add("active");
      const name = (btn.dataset.ctl || "MANUAL").toUpperCase();
      apiSend(`/api/mode?name=${encodeURIComponent(name)}`, `MODE ${name}`);
    });
  });

  document.querySelectorAll("[data-preset]").forEach(btn=>{
    btn.addEventListener("click", ()=>{
      const name = (btn.dataset.preset || "STATIC").toUpperCase();
      apiSend(`/api/preset?name=${encodeURIComponent(name)}`, `PRESET ${name}`);
    });
  });

  const r = $("r"), g = $("g"), b = $("b"), bri = $("bri");
  [r,g,b,bri].forEach(el=>el?.addEventListener("input", syncPreview));
  syncPreview();

  $("btnApply")?.addEventListener("click", ()=>{
    const rv=+r.value, gv=+g.value, bv=+b.value, br=+bri.value;
    apiSend(`/api/rgb?r=${rv}&g=${gv}&b=${bv}`, `RGB ${rv},${gv},${bv}`);
    apiSend(`/api/bri?v=${br}`, `BRI ${br}`);
  });

  $("btnWhite")?.addEventListener("click", ()=>{ r.value=255; g.value=255; b.value=255; syncPreview(); });
  $("btnWarm") ?.addEventListener("click", ()=>{ r.value=255; g.value=120; b.value=40;  syncPreview(); });
  $("btnOff2") ?.addEventListener("click", ()=> apiSend(`/api/power?on=0`, "POWER OFF"));

  window.addEventListener("keydown", (e)=>{
    if (e.repeat) return;
    if (e.key === "Enter"){ e.preventDefault(); $("btnApply")?.click(); }
    else if (e.key.toLowerCase() === "w"){ $("btnWhite")?.click(); }
    else if (e.key.toLowerCase() === "n"){ $("btnWarm")?.click(); }
    else if (e.key.toLowerCase() === "o"){ $("btnOff2")?.click(); }
  });
}

// ---------------- Firebase HMI + Chart ----------------
let chart = null;

function makeChart(){
  const ctx = $("chartHistory")?.getContext("2d");
  if (!ctx) return;
  if (typeof Chart === "undefined") return;

  chart = new Chart(ctx, {
    type: "line",
    data: {
      labels: [],
      datasets: [
        { label: "Lux", data: [], tension: 0.25 },
        { label: "BRI", data: [], tension: 0.25 }
      ]
    },
    options: {
      responsive: true,
      animation: false,
      plugins: { legend: { display: true } },
      scales: { x: { display: false } }
    }
  });
}

// ===== LIVE CHART FROM STATE (PATH_NODE) =====
let _lc_lastTs = 0;
let _lc_labels = [];
let _lc_lux = [];
let _lc_bri = [];
let _lc_timer = null;

function _lc_fmtTs(ts){
  const d = new Date(ts);
  const hh = String(d.getHours()).padStart(2,"0");
  const mm = String(d.getMinutes()).padStart(2,"0");
  const ss = String(d.getSeconds()).padStart(2,"0");
  return `${hh}:${mm}:${ss}`;
}

function _lc_push(ts, lux, bri){
  if (!chart) return;

  if (ts && ts === _lc_lastTs) return;
  _lc_lastTs = ts || Date.now();

  const L = Number(lux);
  const B = Number(bri);

  _lc_labels.push(_lc_fmtTs(_lc_lastTs));
  _lc_lux.push(Number.isFinite(L) ? L : null);
  _lc_bri.push(Number.isFinite(B) ? B : null);

  const MAX = 300;
  if (_lc_labels.length > MAX){
    _lc_labels.splice(0, _lc_labels.length - MAX);
    _lc_lux.splice(0, _lc_lux.length - MAX);
    _lc_bri.splice(0, _lc_bri.length - MAX);
  }

  if (_lc_timer) return;
  _lc_timer = setTimeout(()=>{
    _lc_timer = null;
    chart.data.labels = _lc_labels;
    chart.data.datasets[0].data = _lc_lux;
    chart.data.datasets[1].data = _lc_bri;
    chart.update("none");
  }, 250);
}

function liveChartFromState(s){
  if (!s) return;
  const ts  = s.ts_ms ?? s.ts ?? Date.now();
  const lux = s.lux ?? null;
  const bri = s.bri ?? null;
  _lc_push(ts, lux, bri);
}

function startFirebase(){
  const useFb = $("useFb");
  const wantFb = useFb ? !!useFb.checked : true;

  if (!wantFb){
    setPill("pillFb", "FB: OFF", false);
    setPill("pillState", "STATE: —", false);
    return;
  }

  try{
    if (!window.fbDB) throw new Error("firebase not init");
    setPill("pillFb", "FB: OK", true);

    window.fbDB.ref(PATH_NODE).on("value", (snap)=>{
      const s = snap.val();
      if (!s) return;

      $("vLux").textContent   = (s.lux ?? "—");
      $("vWiFi").textContent  = (s.wifi_rssi ?? s.rssi ?? "—");
      $("vLoRa").textContent  = (s.rssi ?? "—");
      $("vMode").textContent  = (s.mode ?? "—");
      $("vPower").textContent = (s.power ? "ON" : "OFF");
      $("vPreset").textContent= (s.preset ?? "—");
      $("vTs").textContent    = (s.ts_ms ?? "—");

      setPill("pillState", `STATE: OK`, true);

      if (s.mode){
        const m = String(s.mode).toUpperCase();
        document.querySelectorAll(".segbtn").forEach(b=>{
          b.classList.toggle("active", String(b.dataset.ctl||"").toUpperCase() === m);
        });
      }

      // ✅ LIVE chart from STATE
      liveChartFromState(s);
    });

    // ❌ history listener removed to avoid overriding/standing chart

  }catch(e){
    setPill("pillFb", "FB: FAIL", false);
    console.error(e);
  }
}

// ---------------- Init ----------------
window.addEventListener("DOMContentLoaded", ()=>{
  bindControls();
  makeChart();
  startFirebase();
  $("useFb")?.addEventListener("change", startFirebase);
});
