#include "web_console.h"

#include "application.h"
#include "audio_codec.h"
#include "audio_diag.h"
#include "chitu_cert.h"
#include "config.h"
#include "device_state_machine.h"
#include "motor_controller.h"
#include "pet_mode.h"
#include "remote_audio.h"
#include "settings.h"

#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_https_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define TAG "WebConsole"

namespace {

// WebSocket binary frame types (first byte)
constexpr uint8_t kWsMicIn = 0x01;     // phone -> robot: 16 kHz int16 mono
constexpr uint8_t kWsVideo = 0x10;     // robot -> phone: JPEG frame
constexpr uint8_t kWsVoice = 0x20;     // robot -> phone: Chitu's voice, output sample rate
constexpr uint8_t kWsRoom = 0x21;      // robot -> phone: room sound, 16 kHz
constexpr int kVideoIntervalMs = 110;  // ~9 fps
constexpr size_t kRxMax = 8192;

const char kIndexHtml[] = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#07090c"><meta name="apple-mobile-web-app-capable" content="yes">
<title>Chitu</title><style>
:root{--bg:#07090c;--card:rgba(255,255,255,.045);--line:rgba(255,255,255,.08);--t:#eef3f6;--m:#8794a0;--a:#5ff0ff;--a2:#2bb6ff;--glow:0 0 24px rgba(95,240,255,.35);--red:#ff4d6a;--r:22px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;margin:0}
html,body{background:var(--bg);color:var(--t);font:15px/1.4 -apple-system,"SF Pro Text",system-ui,Segoe UI,Roboto,sans-serif;-webkit-user-select:none;user-select:none;overscroll-behavior:none}
body{background:radial-gradient(120% 60% at 50% -10%,#10303a 0,rgba(7,9,12,0) 60%),var(--bg);min-height:100vh}
.app{max-width:520px;margin:0 auto;padding:max(14px,env(safe-area-inset-top)) 16px calc(96px + env(safe-area-inset-bottom))}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}
.logo{display:flex;align-items:center;gap:10px;font-weight:700;font-size:21px;letter-spacing:.5px}
.eyes{display:flex;gap:4px;padding:7px 8px;border-radius:12px;background:#0d1418;border:1px solid var(--line);box-shadow:var(--glow)}
.eyes i{width:11px;height:13px;border-radius:4px;background:var(--a);box-shadow:0 0 8px var(--a);animation:blink 5s infinite}
@keyframes blink{0%,45%,49%,100%{transform:scaleY(1)}47%{transform:scaleY(.1)}}
.pill{display:flex;align-items:center;gap:7px;font-size:12.5px;color:var(--m);padding:7px 12px;border-radius:99px;background:var(--card);border:1px solid var(--line)}
.dot{width:7px;height:7px;border-radius:50%;background:#55606a}.on .dot{background:#3dffa8;box-shadow:0 0 8px #3dffa8}
.cam{position:relative;border-radius:var(--r);overflow:hidden;aspect-ratio:4/3;background:#000;border:1px solid var(--line);box-shadow:0 20px 50px rgba(0,0,0,.5)}
.cam img{width:100%;height:100%;object-fit:cover;display:block}
.cam .msg{position:absolute;inset:0;display:flex;flex-direction:column;gap:10px;align-items:center;justify-content:center;color:var(--m);font-size:14px}
.spin{width:26px;height:26px;border-radius:50%;border:2.5px solid rgba(95,240,255,.2);border-top-color:var(--a);animation:sp 1s linear infinite}@keyframes sp{to{transform:rotate(360deg)}}
.cam .tools{position:absolute;top:10px;right:10px;display:flex;gap:6px}
.ib{width:36px;height:36px;border-radius:12px;border:1px solid rgba(255,255,255,.14);background:rgba(10,14,18,.55);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);color:#fff;display:grid;place-items:center}
.ib svg{width:18px;height:18px}.ib.off{opacity:.5}
.cam .hud{position:absolute;left:0;right:0;bottom:0;padding:26px 12px 10px;display:flex;gap:6px;background:linear-gradient(transparent,rgba(0,0,0,.65))}
.chip{font-size:12px;padding:5px 10px;border-radius:99px;background:rgba(10,14,18,.6);border:1px solid rgba(255,255,255,.12);backdrop-filter:blur(8px)}
.chip.warn{color:#ffc35c;border-color:rgba(255,195,92,.45)}
.panel{display:none;margin-top:16px;animation:fade .25s}.panel.show{display:block}@keyframes fade{from{opacity:0;transform:translateY(6px)}}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:16px}
.seg{display:grid;grid-template-columns:1fr 1fr;background:rgba(255,255,255,.05);border:1px solid var(--line);border-radius:14px;padding:4px;gap:4px}
.seg button{padding:10px;border:0;border-radius:10px;background:none;color:var(--m);font:600 14px inherit;font-family:inherit}
.seg button.act{background:linear-gradient(180deg,#1c2a31,#121b20);color:var(--t);box-shadow:inset 0 0 0 1px rgba(95,240,255,.35),0 4px 14px rgba(0,0,0,.4)}
.joywrap{display:flex;flex-direction:column;align-items:center;gap:14px;padding:18px 0 6px}
.joy{position:relative;width:min(64vw,230px);aspect-ratio:1;border-radius:50%;touch-action:none;
 background:radial-gradient(circle at 50% 50%,rgba(95,240,255,.07) 0 30%,transparent 31%),radial-gradient(circle,#121a1f 0 62%,#0c1216 63%);
 border:1px solid var(--line);box-shadow:inset 0 10px 30px rgba(0,0,0,.6),0 0 0 8px rgba(255,255,255,.02)}
.joy:before{content:"";position:absolute;inset:14%;border-radius:50%;border:1px dashed rgba(95,240,255,.14)}
.joy .tick{position:absolute;width:0;height:0;border:6px solid transparent;opacity:.45}
.tick.n{top:10px;left:calc(50% - 6px);border-bottom-color:var(--a)}.tick.s{bottom:10px;left:calc(50% - 6px);border-top-color:var(--a)}
.tick.w{left:10px;top:calc(50% - 6px);border-right-color:var(--a)}.tick.e{right:10px;top:calc(50% - 6px);border-left-color:var(--a)}
.knob{position:absolute;left:50%;top:50%;width:34%;aspect-ratio:1;margin:-17% 0 0 -17%;border-radius:50%;
 background:radial-gradient(circle at 35% 30%,#bffaff 0,#5ff0ff 22%,#169bc4 70%,#0b5d78 100%);box-shadow:0 10px 25px rgba(0,0,0,.6),0 0 30px rgba(95,240,255,.45);transition:transform .18s cubic-bezier(.2,1.4,.4,1)}
.joy.active .knob{transition:none;box-shadow:0 10px 25px rgba(0,0,0,.6),0 0 44px rgba(95,240,255,.8)}
.row{display:flex;align-items:center;gap:12px;color:var(--m);font-size:13.5px}
.row b{color:var(--t);font-weight:600;min-width:36px;text-align:right}
input[type=range]{flex:1;-webkit-appearance:none;appearance:none;height:6px;border-radius:9px;background:linear-gradient(90deg,var(--a2),var(--a)) 0/var(--p,50%) 100% no-repeat,rgba(255,255,255,.1)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:#fff;box-shadow:0 2px 8px rgba(0,0,0,.5),0 0 0 4px rgba(95,240,255,.25)}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border:0;border-radius:50%;background:#fff}
.stop{width:100%;padding:13px;border:0;border-radius:14px;background:rgba(255,77,106,.12);color:var(--red);font:700 14px inherit;font-family:inherit;letter-spacing:1.5px;border:1px solid rgba(255,77,106,.35)}
.micwrap{display:flex;flex-direction:column;align-items:center;gap:12px;padding:10px 0 4px}
.mic{position:relative;width:118px;height:118px;border-radius:50%;border:0;touch-action:none;color:#04161b;
 background:radial-gradient(circle at 35% 30%,#d6fdff,#5ff0ff 45%,#1aa6cf);box-shadow:0 14px 34px rgba(0,0,0,.55),0 0 34px rgba(95,240,255,.4)}
.mic svg{width:42px;height:42px}
.mic.held{background:radial-gradient(circle at 35% 30%,#ffd6de,#ff5a78 50%,#c3203f);color:#fff}
.mic.held:before,.mic.held:after{content:"";position:absolute;inset:-10px;border-radius:50%;border:2px solid rgba(255,90,120,.5);animation:ring 1.4s infinite}
.mic.held:after{animation-delay:.7s}@keyframes ring{from{transform:scale(.9);opacity:1}to{transform:scale(1.35);opacity:0}}
.cap{color:var(--m);font-size:13px}
.list{margin-top:14px;border-radius:var(--r);overflow:hidden;border:1px solid var(--line);background:var(--card)}
.sw{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 16px;border-top:1px solid var(--line)}
.sw:first-child{border-top:0}.sw span small{display:block;color:var(--m);font-size:12px;margin-top:2px}
.sw input{display:none}.sw i{flex:none;width:46px;height:28px;border-radius:99px;background:rgba(255,255,255,.14);position:relative;transition:.2s}
.sw i:after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:.2s;box-shadow:0 2px 6px rgba(0,0,0,.4)}
.sw input:checked+i{background:linear-gradient(90deg,var(--a2),var(--a))}.sw input:checked+i:after{transform:translateX(18px)}
.note{display:none;margin-top:12px;padding:12px 14px;border-radius:14px;background:rgba(255,195,92,.08);border:1px solid rgba(255,195,92,.3);color:#ffd28a;font-size:13px}
.compose{display:flex;gap:8px;align-items:center;background:rgba(255,255,255,.05);border:1px solid var(--line);border-radius:18px;padding:6px 6px 6px 14px}
.compose input{flex:1;min-width:0;background:none;border:0;outline:0;color:var(--t);font:16px inherit;font-family:inherit;-webkit-user-select:text;user-select:text}
.send{width:42px;height:42px;border-radius:14px;border:0;background:linear-gradient(135deg,var(--a),var(--a2));color:#04161b;display:grid;place-items:center}
.send svg{width:20px;height:20px}
.emo{display:grid;grid-template-columns:repeat(8,1fr);gap:4px;margin-top:12px}
.emo button{font-size:24px;padding:6px 0;border:0;border-radius:12px;background:none}.emo button:active{background:rgba(255,255,255,.08)}
.sent{height:18px;margin-top:8px;color:#3dffa8;font-size:12.5px;text-align:center;opacity:0;transition:.3s}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:14px}
.grid div{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:10px;font-size:11.5px;color:var(--m)}
.grid b{display:block;color:var(--t);font-size:14px;margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.grid .w2{grid-column:span 2}
nav{position:fixed;left:50%;transform:translateX(-50%);bottom:calc(12px + env(safe-area-inset-bottom));width:min(488px,calc(100% - 24px));
 display:grid;grid-template-columns:repeat(4,1fr);padding:6px;border-radius:24px;background:rgba(16,22,26,.78);backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px);border:1px solid var(--line);box-shadow:0 18px 40px rgba(0,0,0,.6);z-index:5}
nav button{display:flex;flex-direction:column;align-items:center;gap:3px;padding:8px 0;border:0;border-radius:18px;background:none;color:var(--m);font:600 11px inherit;font-family:inherit}
nav svg{width:22px;height:22px}nav button.act{color:var(--a);background:rgba(95,240,255,.08)}
@media (min-width:860px){.app{max-width:980px;display:grid;grid-template-columns:1.15fr 1fr;gap:20px;align-items:start}header{grid-column:1/-1}.panel{margin-top:0}.camcol{position:sticky;top:16px}}
</style></head><body><div class="app">
<header><div class="logo"><div class="eyes"><i></i><i></i></div>Chitu</div><div class="pill" id="conn"><span class="dot"></span><span id="connt">Connecting</span></div></header>
<div class="camcol"><div class="cam"><img id="cam" alt="">
<div class="msg" id="cammsg"><div class="spin"></div><span id="cammsgt">Connecting to camera</span></div>
<div class="tools">
<button class="ib" id="cOn" title="Camera"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M23 7l-7 5 7 5V7z"/><rect x="1" y="5" width="15" height="14" rx="2"/></svg></button>
<button class="ib" id="cFlip" title="Flip"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v18M7 8l5-5 5 5M7 16l5 5 5-5"/></svg></button>
<button class="ib" id="cMirror" title="Mirror"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12h18M8 7l-5 5 5 5M16 7l5 5-5 5"/></svg></button></div>
<div class="hud"><span class="chip" id="dist">— cm</span><span class="chip" id="stchip">idle</span></div></div></div>

<div>
<section class="panel show" id="p-drive"><div class="card">
<div class="seg"><button id="mManual" class="act">Manual</button><button id="mAuto">Auto roam</button></div>
<div class="joywrap"><div class="joy" id="joy"><span class="tick n"></span><span class="tick s"></span><span class="tick w"></span><span class="tick e"></span><div class="knob" id="knob"></div></div></div>
<div class="row">Max speed<input id="speed" type="range" min="35" max="100" value="75"><b id="speedv">75%</b></div>
<button class="stop" id="stop" style="margin-top:14px">STOP</button></div></section>

<section class="panel" id="p-talk"><div class="card"><div class="micwrap">
<button class="mic" id="talk"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 10a7 7 0 0 0 14 0M12 17v5M8 22h8"/></svg></button>
<div class="cap" id="talkcap">Hold to talk to Chitu</div></div><div class="note" id="micnote"></div></div>
<div class="list">
<label class="sw"><span>Chitu's voice on phone<small>Hear Chitu's replies here</small></span><input type="checkbox" id="tPhone" checked><i></i></label>
<label class="sw"><span>Robot speaker<small>Also play on Chitu</small></span><input type="checkbox" id="tSpk" checked><i></i></label>
<label class="sw"><span>Hear the room<small>Listen through Chitu's mic</small></span><input type="checkbox" id="tRoom"><i></i></label>
<label class="sw"><span>Phone mic always on<small>Say the wake word to your phone</small></span><input type="checkbox" id="tMic"><i></i></label></div></section>

<section class="panel" id="p-msg"><div class="card">
<div class="compose"><input id="text" maxlength="120" placeholder="Message for Chitu's screen" enterkeyhint="send"><button class="send" id="send"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M22 2L11 13M22 2l-7 20-4-9-9-4z"/></svg></button></div>
<div class="emo" id="emo"></div><div class="sent" id="sent">Sent to Chitu ✓</div></div>
<div class="list"><label class="sw"><span>Chitu says it out loud<small>Reads your message and reacts</small></span><input type="checkbox" id="sayit"><i></i></label></div></section>

<section class="panel" id="p-pet"><div class="list">
<label class="sw"><span>Pet mode<small>Looks around, makes faces, naps</small></span><input type="checkbox" id="tPet" checked><i></i></label>
<label class="sw"><span>Chatty<small>Sometimes speaks up on its own</small></span><input type="checkbox" id="tChat" checked><i></i></label></div>
<div class="card" style="margin-top:14px"><div class="row">Volume<input id="vol" type="range" min="0" max="100" value="80"><b id="volv">80</b></div></div>
<div class="grid"><div>State<b id="state">—</b></div><div>Wi-Fi<b id="rssi">—</b></div><div>Path<b id="obs">—</b></div>
<div>Mic<b id="mic">—</b></div><div class="w2">Wake word<b id="wake">—</b></div></div></section>
</div></div>

<nav><button class="act" data-p="drive"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3.5" fill="currentColor"/></svg>Drive</button>
<button data-p="talk"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 10a7 7 0 0 0 14 0M12 17v4"/></svg>Talk</button>
<button data-p="msg"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>Message</button>
<button data-p="pet"><svg viewBox="0 0 24 24" fill="currentColor"><circle cx="6" cy="9" r="2.2"/><circle cx="10" cy="5.5" r="2.2"/><circle cx="14" cy="5.5" r="2.2"/><circle cx="18" cy="9" r="2.2"/><path d="M12 11c-3 0-6 4.5-6 7a3 3 0 0 0 3 3c1.2 0 2-.6 3-.6s1.8.6 3 .6a3 3 0 0 0 3-3c0-2.5-3-7-6-7z"/></svg>Chitu</button></nav>
<script>
const $=id=>document.getElementById(id);
const cmd=q=>fetch('/cmd?'+q).catch(()=>{});
const buzz=ms=>{try{navigator.vibrate&&navigator.vibrate(ms)}catch(e){}};
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('act',x===b));
 document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('show',p.id==='p-'+b.dataset.p));AC()});
function fill(r){r.style.setProperty('--p',((r.value-r.min)/(r.max-r.min)*100)+'%')}
// ---------- audio ----------
let ac=null,rates={ir:16000,or:24000};
function AC(){if(!ac){const C=window.AudioContext||window.webkitAudioContext;ac=new C()}if(ac.state==='suspended')ac.resume();return ac}
document.addEventListener('pointerdown',()=>AC(),{once:true});
const players={v:{next:0},r:{next:0}};
function play(view,rate,pl){if(!ac||ac.state!=='running')return;const n=(view.byteLength-1)>>1;if(n<=0)return;
 const b=ac.createBuffer(1,n,rate),ch=b.getChannelData(0);for(let i=0;i<n;i++)ch[i]=view.getInt16(1+i*2,true)/32768;
 const s=ac.createBufferSource();s.buffer=b;s.connect(ac.destination);const now=ac.currentTime;
 if(pl.next<now+0.03||pl.next>now+0.8)pl.next=now+0.15;s.start(pl.next);pl.next+=b.duration}
let mic=null,holding=false,micAlways=false,dsAcc=0,dsSum=0,dsCnt=0;
async function micStart(){const c=AC();if(mic)return true;
 if(!navigator.mediaDevices||!window.isSecureContext){note('The microphone needs the secure page. Open https://'+location.hostname+'/');return false}
 try{const st=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true,noiseSuppression:true,autoGainControl:true}});
  const src=c.createMediaStreamSource(st),p=c.createScriptProcessor(2048,1,1);
  p.onaudioprocess=e=>{if(!(holding||micAlways)||!ws||ws.readyState!==1)return;const d=e.inputBuffer.getChannelData(0),step=c.sampleRate/16000,out=[];
   for(let i=0;i<d.length;i++){dsSum+=d[i];dsCnt++;if(++dsAcc>=step){dsAcc-=step;out.push(dsSum/dsCnt);dsSum=0;dsCnt=0}}
   const buf=new DataView(new ArrayBuffer(1+out.length*2));buf.setUint8(0,1);
   for(let i=0;i<out.length;i++){let v=Math.max(-1,Math.min(1,out[i]*1.4));buf.setInt16(1+i*2,v*32767,true)}ws.send(buf.buffer)};
  src.connect(p);p.connect(c.destination);mic={st,p};note('');return true}
 catch(e){note('Microphone blocked: allow microphone access for this page in your browser settings.');return false}}
function note(t){$('micnote').textContent=t;$('micnote').style.display=t?'block':'none'}
const talk=$('talk');
talk.addEventListener('pointerdown',async e=>{e.preventDefault();talk.setPointerCapture(e.pointerId);if(!await micStart())return;holding=true;buzz(15);talk.classList.add('held');$('talkcap').textContent='Listening… release to send';cmd('a=talk&on=1')});
['pointerup','pointercancel','lostpointercapture'].forEach(ev=>talk.addEventListener(ev,()=>{if(!holding)return;talk.classList.remove('held');$('talkcap').textContent='Hold to talk to Chitu';setTimeout(()=>{holding=false;cmd('a=talk&on=0')},350)}));
function sw(id,key,cb){const el=$(id);el.onchange=async()=>{AC();if(cb&&!(await cb(el.checked))){el.checked=!el.checked;return}cmd('a='+key+'&on='+(el.checked?1:0))};return el}
const tPhone=sw('tPhone','audio'),tSpk=sw('tSpk','spk'),tRoom=sw('tRoom','room');
sw('tMic','mic',async on=>{if(on&&!await micStart())return false;micAlways=on;return true});
const tPet=sw('tPet','pet'),tChat=sw('tChat','chatty');
// ---------- camera + audio stream ----------
let ws=null,camUrl=null,camOn=true;const cam=$('cam');
function camMsg(t,spin){$('cammsg').style.display=t?'flex':'none';$('cammsgt').textContent=t||'';$('cammsg').querySelector('.spin').style.display=spin?'block':'none'}
function connect(){ws=new WebSocket('wss://'+location.host+'/ws');ws.binaryType='arraybuffer';
 ws.onopen=()=>{if(camOn)camMsg('Starting camera',1);cmd('a=audio&on='+(tPhone.checked?1:0));cmd('a=spk&on='+(tSpk.checked?1:0));cmd('a=room&on='+(tRoom.checked?1:0));cmd('a=video&on='+(camOn?1:0))};
 ws.onmessage=e=>{if(typeof e.data==='string')return;const v=new DataView(e.data),t=v.getUint8(0);
  if(t===0x10){const u=URL.createObjectURL(new Blob([new Uint8Array(e.data,1)],{type:'image/jpeg'}));cam.onload=()=>{if(camUrl)URL.revokeObjectURL(camUrl);camUrl=u};cam.src=u;camMsg('')}
  else if(t===0x20)play(v,rates.or,players.v);else if(t===0x21)play(v,rates.ir,players.r)};
 ws.onclose=()=>{camMsg('Reconnecting',1);setTimeout(connect,2000)}}
connect();setInterval(()=>{if(ws&&ws.readyState===1)ws.send('p')},1000);
$('cOn').onclick=()=>{camOn=!camOn;$('cOn').classList.toggle('off',!camOn);cmd('a=video&on='+(camOn?1:0));if(!camOn)camMsg('Camera paused',0)};
let camV=0,camH=0;$('cFlip').onclick=()=>{camV^=1;cmd('a=cam&v='+camV)};$('cMirror').onclick=()=>{camH^=1;cmd('a=cam&h='+camH)};
// ---------- joystick ----------
const joy=$('joy'),knob=$('knob');let jx=0,jy=0,jActive=false,jTimer=null,lastSector=-1;
function jSend(){cmd('a=joy&x='+Math.round(jx*100)+'&y='+Math.round(jy*100)+'&s='+$('speed').value)}
function jMove(e){const r=joy.getBoundingClientRect(),R=r.width/2,max=R*0.62;let dx=e.clientX-(r.left+R),dy=e.clientY-(r.top+R);
 const d=Math.hypot(dx,dy);if(d>max){dx*=max/d;dy*=max/d}knob.style.transform='translate('+dx+'px,'+dy+'px)';jx=dx/max;jy=-dy/max;
 const mag=Math.hypot(jx,jy),sec=mag<0.25?-1:Math.round((Math.atan2(jy,jx)/(Math.PI/4)+8))%8;if(sec!==lastSector){lastSector=sec;buzz(8);jSend()}}
joy.addEventListener('pointerdown',e=>{e.preventDefault();joy.setPointerCapture(e.pointerId);jActive=true;joy.classList.add('active');setMode(false,true);jMove(e);jSend();jTimer=setInterval(jSend,150)});
joy.addEventListener('pointermove',e=>{if(jActive)jMove(e)});
function jEnd(){if(!jActive)return;jActive=false;joy.classList.remove('active');clearInterval(jTimer);jx=jy=0;lastSector=-1;knob.style.transform='';cmd('a=stop')}
['pointerup','pointercancel','lostpointercapture'].forEach(ev=>joy.addEventListener(ev,jEnd));
$('stop').onclick=()=>{jEnd();setMode(false);cmd('a=stop');buzz(30)};
function setMode(auto,quiet){$('mAuto').classList.toggle('act',auto);$('mManual').classList.toggle('act',!auto);if(!quiet)cmd('a=roam&on='+(auto?1:0))}
$('mAuto').onclick=()=>setMode(true);$('mManual').onclick=()=>setMode(false);
const sp=$('speed'),vol=$('vol');[sp,vol].forEach(fill);
sp.oninput=()=>{$('speedv').textContent=sp.value+'%';fill(sp)};
vol.oninput=()=>{$('volv').textContent=vol.value;fill(vol)};vol.onchange=()=>cmd('a=volume&v='+vol.value);
document.addEventListener('visibilitychange',()=>{if(document.hidden){jEnd();holding=false}});
// ---------- messages ----------
'😀😂😍🥰😘😎🤔😴😢😡😱🤖👍👋🙏❤🔥🎉⭐🍕🐶🌙☕🎂'.match(/\p{Extended_Pictographic}/gu).forEach(c=>{const b=document.createElement('button');b.textContent=c;b.onclick=()=>{$('text').value+=c};$('emo').appendChild(b)});
function sendMsg(){let t=$('text').value.trim();if(!t)return;t=t.replace(/[️‍]|[\u{1F3FB}-\u{1F3FF}]/gu,'');
 fetch('/msg?say='+($('sayit').checked?1:0),{method:'POST',body:t}).then(r=>{if(r.ok){$('text').value='';$('sent').style.opacity=1;buzz(12);setTimeout(()=>$('sent').style.opacity=0,1800)}})}
$('send').onclick=sendMsg;$('text').addEventListener('keydown',e=>{if(e.key==='Enter')sendMsg()});
// ---------- status ----------
let fails=0,volSet=false,synced=false;
async function poll(){try{const r=await fetch('/status',{cache:'no-store'});const s=await r.json();fails=0;
 $('conn').className='pill on';$('connt').textContent=s.rssi+' dBm';rates.ir=s.ir;rates.or=s.or;camV=s.cam_v|0;camH=s.cam_h|0;
 const mm=s.m.distance_mm,d=$('dist');
 if(mm<0){d.textContent='no sensor';d.className='chip'}else{d.textContent=(mm>=2000?'> 2 m':(mm>=1000?(mm/1000).toFixed(1)+' m':Math.round(mm/10)+' cm'));d.className='chip'+(mm<300?' warn':'')}
 const st=s.m.roaming?'roaming':(s.m.dancing?'dancing':(s.m.moving?'moving':s.st));$('state').textContent=st;$('stchip').textContent=s.m.obstacle_stop?'blocked':st;$('stchip').className='chip'+(s.m.obstacle_stop?' warn':'');
 $('obs').textContent=s.m.obstacle_stop?'blocked':'clear';$('rssi').textContent=s.rssi+' dBm';
 $('mic').textContent=s.phone_mic?'phone':(s.mic+(s.mic_clip?' clip':''));$('wake').textContent=s.wake;
 if(!jActive){$('mAuto').classList.toggle('act',s.m.roaming);$('mManual').classList.toggle('act',!s.m.roaming)}
 if(!volSet&&s.volume>=0){vol.value=s.volume;$('volv').textContent=s.volume;fill(vol);volSet=true}
 if(!synced){tPet.checked=s.pet;tChat.checked=s.chatty;synced=true}
}catch(e){if(++fails>2){$('conn').className='pill';$('connt').textContent='Offline'}}
setTimeout(poll,800)}
poll();
</script></body></html>)HTML";

bool QueryValue(const char* query, const char* key, char* out, size_t out_len) {
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

bool QueryOn(const char* query) {
    char on[4] = {0};
    return QueryValue(query, "on", on, sizeof(on)) && on[0] == '1';
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}
}  // namespace

std::string WebConsole::Url() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return "";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "https://" IPSTR "/", IP2STR(&ip.ip));
    return buf;
}

bool WebConsole::RecentlyUsed() const {
    return client_fd_.load() >= 0 ||
           esp_timer_get_time() - last_use_us_.load() < 60LL * 1000 * 1000;
}

void WebConsole::Start(MotorController* motors, AudioCodec* codec, PetMode* pet) {
    if (secure_server_ != nullptr) {
        return;  // already running (Wi-Fi reconnects keep the same servers)
    }
    motors_ = motors;
    codec_ = codec;
    pet_ = pet;
    if (!remote_audio::Init()) {
        ESP_LOGE(TAG, "No memory for phone audio buffers");
    }
    rx_buf_ = static_cast<uint8_t*>(heap_caps_malloc(kRxMax, MALLOC_CAP_SPIRAM));

    // Secure server: the page, commands and the camera/audio WebSocket
    httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
    ssl.servercert = reinterpret_cast<const uint8_t*>(kChituCertPem);
    ssl.servercert_len = sizeof(kChituCertPem);
    ssl.prvtkey_pem = reinterpret_cast<const uint8_t*>(kChituKeyPem);
    ssl.prvtkey_len = sizeof(kChituKeyPem);
    ssl.port_secure = WEB_CONSOLE_PORT_SECURE;
    ssl.httpd.ctrl_port = 32780;
    ssl.httpd.max_open_sockets = 5;
    ssl.httpd.max_uri_handlers = 8;
    ssl.httpd.lru_purge_enable = true;
    ssl.httpd.recv_wait_timeout = 10;
    ssl.httpd.send_wait_timeout = 10;
    if (httpd_ssl_start(&secure_server_, &ssl) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the secure web console");
        secure_server_ = nullptr;
        return;
    }
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = this};
    httpd_uri_t cmd = {.uri = "/cmd", .method = HTTP_GET, .handler = HandleCmd, .user_ctx = this};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = HandleStatus, .user_ctx = this};
    httpd_uri_t msg = {.uri = "/msg", .method = HTTP_POST, .handler = HandleMsg, .user_ctx = this};
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = HandleWs, .user_ctx = this};
    ws.is_websocket = true;
    httpd_register_uri_handler(secure_server_, &index);
    httpd_register_uri_handler(secure_server_, &cmd);
    httpd_register_uri_handler(secure_server_, &status);
    httpd_register_uri_handler(secure_server_, &msg);
    httpd_register_uri_handler(secure_server_, &ws);

    // Plain http://<ip>/ just sends the phone to the secure page
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_CONSOLE_PORT;
    cfg.ctrl_port = 32781;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 3584;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&redirect_server_, &cfg) == ESP_OK) {
        httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = HandleRedirect, .user_ctx = this};
        httpd_register_uri_handler(redirect_server_, &any);
    } else {
        redirect_server_ = nullptr;
    }

    xTaskCreate([](void* arg) { static_cast<WebConsole*>(arg)->MediaLoop(); }, "chitu_media", 4096,
                this, 4, nullptr);
    ESP_LOGI(TAG, "Web console running at %s", Url().c_str());
}

esp_err_t WebConsole::HandleRedirect(httpd_req_t* req) {
    char host[64] = {0};
    std::string location = "https://";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        char* colon = strchr(host, ':');
        if (colon != nullptr) *colon = '\0';
        location += host;
        location += "/";
    } else {
        location = static_cast<WebConsole*>(req->user_ctx)->Url();
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    httpd_resp_set_type(req, "text/html");
    std::string body = "<a href=\"" + location + "\">Open Chitu console</a>";
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t WebConsole::HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kIndexHtml, sizeof(kIndexHtml) - 1);
}

esp_err_t WebConsole::HandleCmd(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    char query[128] = {0};
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !QueryValue(query, "a", action, sizeof(action))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing a=");
        return ESP_FAIL;
    }
    self->last_use_us_ = esp_timer_get_time();
    if (self->pet_ != nullptr) {
        self->pet_->Poke();
    }

    bool ok = true;
    auto& app = Application::GetInstance();
    if (strcmp(action, "drive") == 0) {
        char dir[12] = {0}, speed_s[8] = {0};
        QueryValue(query, "d", dir, sizeof(dir));
        int speed = QueryValue(query, "s", speed_s, sizeof(speed_s)) ? atoi(speed_s) : 70;
        speed = std::clamp(speed, 1, 100);
        self->motors_->StopRoam();
        self->motors_->StopDance();
        // Short duration: the page re-sends every 200 ms while a button is held,
        // so the robot stops by itself if the phone disconnects (dead-man switch).
        ok = self->motors_->DriveNamed(dir, speed, 600);
    } else if (strcmp(action, "joy") == 0) {
        // Joystick: x/y -100..100, s = max speed. Both wheels share one speed line on
        // this board, so the stick picks one of 8 moves (forward, curves, spins, back)
        // and the distance from the centre sets the speed.
        char xs[8] = {0}, ys[8] = {0}, ss[8] = {0};
        QueryValue(query, "x", xs, sizeof(xs));
        QueryValue(query, "y", ys, sizeof(ys));
        int max_speed = QueryValue(query, "s", ss, sizeof(ss)) ? std::clamp(atoi(ss), 35, 100) : 75;
        float x = std::clamp(atoi(xs), -100, 100) / 100.0f;
        float y = std::clamp(atoi(ys), -100, 100) / 100.0f;
        float mag = std::min(1.0f, sqrtf(x * x + y * y));
        self->motors_->StopRoam();
        self->motors_->StopDance();
        if (mag < 0.25f) {
            self->motors_->Stop();
        } else {
            using D = MotorController::Dir;
            // sector 0 = right, counter-clockwise in 45 degree steps
            int sector = (int)lroundf(atan2f(y, x) / (float)(M_PI / 4)) & 7;
            static const D kMoves[8][2] = {
                {D::kForward, D::kBackward},   // right: spin right
                {D::kForward, D::kStop},       // forward-right: curve
                {D::kForward, D::kForward},    // forward
                {D::kStop, D::kForward},       // forward-left: curve
                {D::kBackward, D::kForward},   // left: spin left
                {D::kStop, D::kBackward},      // back-left
                {D::kBackward, D::kBackward},  // back
                {D::kBackward, D::kStop},      // back-right
            };
            int speed = 35 + (int)lroundf((max_speed - 35) * (mag - 0.25f) / 0.75f);
            ok = self->motors_->Drive(kMoves[sector][0], kMoves[sector][1], speed, 600);
        }
    } else if (strcmp(action, "stop") == 0) {
        self->motors_->Stop();
    } else if (strcmp(action, "roam") == 0) {
        if (QueryOn(query)) {
            ok = self->motors_->StartRoam(ROAM_MAX_MINUTES);
        } else {
            self->motors_->Stop();
        }
    } else if (strcmp(action, "volume") == 0) {
        char v[8] = {0};
        if (self->codec_ != nullptr && QueryValue(query, "v", v, sizeof(v))) {
            self->codec_->SetOutputVolume(std::clamp(atoi(v), 0, 100));
        }
    } else if (strcmp(action, "talk") == 0) {
        // Push-to-talk from the phone: the phone mic feeds the AI while held
        if (QueryOn(query)) {
            app.StartListening();
        } else {
            app.StopListening();
        }
    } else if (strcmp(action, "audio") == 0) {
        remote_audio::phone_listen = QueryOn(query);
    } else if (strcmp(action, "spk") == 0) {
        remote_audio::robot_speaker = QueryOn(query);
    } else if (strcmp(action, "room") == 0) {
        remote_audio::room_listen = QueryOn(query);
    } else if (strcmp(action, "mic") == 0) {
        // "Phone mic always": nothing to switch here, the phone simply keeps streaming
    } else if (strcmp(action, "video") == 0) {
        self->video_on_ = QueryOn(query);
    } else if (strcmp(action, "pet") == 0) {
        if (self->pet_ != nullptr) self->pet_->SetEnabled(QueryOn(query));
    } else if (strcmp(action, "chatty") == 0) {
        if (self->pet_ != nullptr) self->pet_->SetChatty(QueryOn(query));
    } else if (strcmp(action, "cam") == 0) {
        // Camera orientation: v=vertical flip, h=horizontal mirror (0/1), saved to NVS
        sensor_t* s = esp_camera_sensor_get();
        char v[4] = {0}, h[4] = {0};
        if (s == nullptr) {
            ok = false;
        } else {
            Settings settings("chitu", true);
            if (QueryValue(query, "v", v, sizeof(v))) {
                int on = v[0] == '1' ? 1 : 0;
                s->set_vflip(s, on);
                settings.SetInt("cam_vflip", on);
            }
            if (QueryValue(query, "h", h, sizeof(h))) {
                int on = h[0] == '1' ? 1 : 0;
                s->set_hmirror(s, on);
                settings.SetInt("cam_hmirror", on);
            }
        }
    } else {
        ok = false;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t WebConsole::HandleMsg(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    char query[32] = {0};
    char say[4] = {0};
    bool say_it = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                  QueryValue(query, "say", say, sizeof(say)) && say[0] == '1';
    int len = std::min<int>(req->content_len, 480);
    std::string text(len, '\0');
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, text.data() + got, len - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no text");
            return ESP_FAIL;
        }
        got += r;
    }
    self->last_use_us_ = esp_timer_get_time();
    if (self->pet_ != nullptr) {
        self->pet_->Poke();
    }
    if (!text.empty() && self->on_message_) {
        self->on_message_(text, say_it);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t WebConsole::HandleStatus(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    int rssi = 0;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    int volume = self->codec_ ? self->codec_->output_volume() : -1;
    sensor_t* cam = esp_camera_sensor_get();
    auto state = Application::GetInstance().GetDeviceState();
    std::string json =
        "{\"m\":" + self->motors_->StatusJson() + ",\"rssi\":" + std::to_string(rssi) +
        ",\"volume\":" + std::to_string(volume) + ",\"camera\":" + (cam ? "true" : "false") +
        ",\"cam_v\":" + std::to_string(cam ? cam->status.vflip : 0) +
        ",\"cam_h\":" + std::to_string(cam ? cam->status.hmirror : 0) +
        ",\"mic\":" + std::to_string(audio_diag::mic_rms.load()) +
        ",\"mic_clip\":" + (audio_diag::mic_clipped > 0 ? "true" : "false") +
        ",\"phone_mic\":" + (remote_audio::PhoneMicActive() ? "true" : "false") +
        ",\"ir\":" + std::to_string(self->codec_ ? self->codec_->input_sample_rate() : 16000) +
        ",\"or\":" + std::to_string(self->codec_ ? self->codec_->output_sample_rate() : 24000) +
        ",\"pet\":" + (self->pet_ && self->pet_->enabled() ? "true" : "false") +
        ",\"chatty\":" + (self->pet_ && self->pet_->chatty() ? "true" : "false") +
        ",\"st\":\"" + JsonEscape(DeviceStateMachine::GetStateName(state)) + "\"" +
        ",\"wake\":\"" + JsonEscape(audio_diag::WakeWords()) + "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

// ---------------------------------------------------------------------------
// WebSocket: camera + audio
// ---------------------------------------------------------------------------
esp_err_t WebConsole::HandleWs(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        // Handshake done: the newest phone takes over
        int old = self->client_fd_.exchange(fd);
        if (old >= 0 && old != fd) {
            httpd_sess_trigger_close(self->secure_server_, old);
        }
        self->last_rx_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Phone connected (socket %d)", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.len > kRxMax || self->rx_buf_ == nullptr) {
        return ESP_FAIL;
    }
    frame.payload = self->rx_buf_;
    if (frame.len > 0 && httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (fd == self->client_fd_.load()) {
        self->last_rx_us_ = esp_timer_get_time();
    }
    if (frame.type == HTTPD_WS_TYPE_BINARY && frame.len > 1 && frame.payload[0] == kWsMicIn) {
        remote_audio::phone_mic.Push(frame.payload + 1, (frame.len - 1) & ~1u);
        remote_audio::last_phone_mic_us = esp_timer_get_time();
    }
    return ESP_OK;
}

bool WebConsole::EnsureTx(size_t len) {
    if (tx_buf_ != nullptr && tx_cap_ >= len + 1) {
        return true;
    }
    if (tx_buf_ != nullptr) heap_caps_free(tx_buf_);
    tx_cap_ = len + 1 + 8 * 1024;
    tx_buf_ = static_cast<uint8_t*>(heap_caps_malloc(tx_cap_, MALLOC_CAP_SPIRAM));
    if (tx_buf_ == nullptr) {
        tx_cap_ = 0;
        return false;
    }
    return true;
}

bool WebConsole::SendTx(int fd, size_t len) {
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = tx_buf_;
    frame.len = len + 1;
    // Queued to the server task (TLS is not shared between tasks) and waits until sent
    return httpd_ws_send_data(secure_server_, fd, &frame) == ESP_OK;
}

bool WebConsole::SendBinary(int fd, uint8_t type, const void* data, size_t len) {
    if (!EnsureTx(len)) {
        return false;
    }
    tx_buf_[0] = type;
    memcpy(tx_buf_ + 1, data, len);
    return SendTx(fd, len);
}

void WebConsole::DropClient(const char* why) {
    int fd = client_fd_.exchange(-1);
    if (fd >= 0) {
        ESP_LOGI(TAG, "Phone disconnected (%s)", why);
        // only close it if that socket is still our WebSocket (fd numbers get reused)
        if (httpd_ws_get_fd_info(secure_server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_sess_trigger_close(secure_server_, fd);
        }
    }
    remote_audio::ResetRouting();  // robot mic + speaker back to normal
    video_on_ = true;
}

void WebConsole::MediaLoop() {
    // up to ~330 ms of audio per message so audio keeps up while video frames are sent
    constexpr size_t kPcmBytes = 16000;
    auto* pcm = static_cast<int16_t*>(heap_caps_malloc(kPcmBytes, MALLOC_CAP_SPIRAM));
    if (pcm == nullptr) {
        ESP_LOGE(TAG, "No memory for media task");
        vTaskDelete(nullptr);
        return;
    }
    int64_t last_frame_us = 0;
    while (true) {
        int fd = client_fd_.load();
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (httpd_ws_get_fd_info(secure_server_, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            DropClient("closed");
            continue;
        }
        if (now - last_rx_us_.load() > 5LL * 1000 * 1000) {
            DropClient("no heartbeat");
            continue;
        }
        bool sent = false;
        bool ok = true;

        // Audio first: it is small and latency matters
        size_t n = remote_audio::voice_out.Pop(pcm, kPcmBytes);
        if (n > 0) {
            ok = SendBinary(fd, kWsVoice, pcm, n);
            sent = true;
        }
        n = remote_audio::room_out.Pop(pcm, kPcmBytes);
        if (ok && n > 0) {
            ok = SendBinary(fd, kWsRoom, pcm, n);
            sent = true;
        }

        if (ok && video_on_ && (now - last_frame_us) / 1000 >= kVideoIntervalMs &&
            esp_camera_sensor_get() != nullptr) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb != nullptr) {
                last_frame_us = now;
                if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
                    // copy, then return the buffer at once so AI photos never wait on Wi-Fi
                    size_t len = fb->len;
                    bool fits = EnsureTx(len);
                    if (fits) {
                        tx_buf_[0] = kWsVideo;
                        memcpy(tx_buf_ + 1, fb->buf, len);
                    }
                    esp_camera_fb_return(fb);
                    if (fits) {
                        ok = SendTx(fd, len);
                    }
                } else {
                    esp_camera_fb_return(fb);
                }
                sent = true;
            }
        }
        if (!ok) {
            DropClient("send failed");
            continue;
        }
        if (!sent) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}
