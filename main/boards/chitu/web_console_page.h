#pragma once
// The phone console page (served by web_console.cc). One file: HTML, CSS and script.
//
// Main screen (never scrolls): live camera with status chips, joystick, hold-to-talk,
// STOP, stop-talking, roam, speed and volume. Everything else opens as a sheet on top:
// Alarms, Message, Chitu (personality, voice, audio routes) and Setup.
namespace web_page {

inline const char kIndexHtml[] = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#07090c"><meta name="apple-mobile-web-app-capable" content="yes">
<title>Chitu</title><style>
:root{--bg:#07090c;--card:rgba(255,255,255,.045);--line:rgba(255,255,255,.08);--t:#eef3f6;--m:#8794a0;--a:#5ff0ff;--a2:#2bb6ff;--glow:0 0 24px rgba(95,240,255,.35);--red:#ff4d6a;--ok:#3dffa8;--warn:#ffc35c;--r:18px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;margin:0}
html,body{height:100%;overflow:hidden;background:var(--bg);color:var(--t);font:14px/1.35 -apple-system,"SF Pro Text",system-ui,Segoe UI,Roboto,sans-serif;-webkit-user-select:none;user-select:none;overscroll-behavior:none}
body{background:radial-gradient(120% 60% at 50% -10%,#10303a 0,rgba(7,9,12,0) 60%),var(--bg)}
button{font-family:inherit}
/* ---------- main screen: header / camera / controls, always one screen ---------- */
.app{height:100vh;height:100dvh;display:grid;grid-template-columns:minmax(0,1fr);grid-template-rows:auto auto minmax(0,1fr);gap:8px;
 padding:max(8px,env(safe-area-inset-top)) max(10px,env(safe-area-inset-right)) max(8px,env(safe-area-inset-bottom)) max(10px,env(safe-area-inset-left));max-width:1100px;margin:0 auto}
header{display:flex;align-items:center;gap:8px;min-width:0}
.logo{display:flex;align-items:center;gap:8px;font-weight:700;font-size:17px;letter-spacing:.4px;margin-right:auto}
.eyes{display:flex;gap:3px;padding:5px 6px;border-radius:10px;background:#0d1418;border:1px solid var(--line);box-shadow:var(--glow)}
.eyes i{width:8px;height:10px;border-radius:3px;background:var(--a);box-shadow:0 0 6px var(--a);animation:blink 5s infinite}
@keyframes blink{0%,45%,49%,100%{transform:scaleY(1)}47%{transform:scaleY(.1)}}
.pill{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--m);padding:5px 9px;border-radius:99px;background:var(--card);border:1px solid var(--line);white-space:nowrap}
.dot{width:7px;height:7px;border-radius:50%;background:#55606a}.on .dot{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.hb{width:34px;height:34px;border-radius:11px;border:1px solid var(--line);background:var(--card);color:var(--t);display:grid;place-items:center;flex:none}
.hb svg{width:18px;height:18px}.hb.ring{color:var(--red);border-color:rgba(255,77,106,.5);animation:pulse 1s infinite}
.camwrap{min-height:0;min-width:0;display:flex;align-items:center;justify-content:center}
.cam{position:relative;width:min(100%,calc(46dvh * 4 / 3));aspect-ratio:4/3;border-radius:var(--r);overflow:hidden;background:#000;border:1px solid var(--line);box-shadow:0 14px 40px rgba(0,0,0,.5)}
.cam img{width:100%;height:100%;object-fit:cover;display:block}
.cam .msg{position:absolute;inset:0;display:flex;flex-direction:column;gap:8px;align-items:center;justify-content:center;color:var(--m);font-size:13px;text-align:center;padding:14px}
.spin{width:24px;height:24px;border-radius:50%;border:2.5px solid rgba(95,240,255,.2);border-top-color:var(--a);animation:sp 1s linear infinite}@keyframes sp{to{transform:rotate(360deg)}}
.play{width:52px;height:52px;border-radius:50%;border:0;background:radial-gradient(circle at 35% 30%,#d6fdff,#5ff0ff 45%,#1aa6cf);color:#04161b;display:grid;place-items:center;box-shadow:0 0 26px rgba(95,240,255,.4)}
.play svg{width:22px;height:22px;margin-left:3px}
.cam .tools{position:absolute;top:8px;right:8px;display:flex;gap:6px}
.ib{width:32px;height:32px;border-radius:10px;border:1px solid rgba(255,255,255,.14);background:rgba(10,14,18,.55);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);color:#fff;display:grid;place-items:center}
.ib svg{width:16px;height:16px}.ib.off{opacity:.5}
.cam .hud{position:absolute;left:0;right:0;bottom:0;padding:20px 8px 7px;display:flex;flex-wrap:wrap;gap:5px;background:linear-gradient(transparent,rgba(0,0,0,.65))}
.chip{font-size:11.5px;padding:4px 8px;border-radius:99px;background:rgba(10,14,18,.6);border:1px solid rgba(255,255,255,.12);backdrop-filter:blur(8px);white-space:nowrap}
.chip.warn{color:var(--warn);border-color:rgba(255,195,92,.45)}.chip.ok{color:var(--ok);border-color:rgba(61,255,168,.4)}.chip.bad{color:#fff;background:rgba(255,77,106,.75);border-color:var(--red)}
.ctl{display:grid;grid-template-columns:auto minmax(0,1fr);gap:10px;align-items:center;align-self:center}
.joy{position:relative;width:clamp(118px,min(40vw,27dvh),210px);aspect-ratio:1;border-radius:50%;touch-action:none;
 background:radial-gradient(circle at 50% 50%,rgba(95,240,255,.07) 0 30%,transparent 31%),radial-gradient(circle,#121a1f 0 62%,#0c1216 63%);
 border:1px solid var(--line);box-shadow:inset 0 10px 30px rgba(0,0,0,.6),0 0 0 6px rgba(255,255,255,.02)}
.joy:before{content:"";position:absolute;inset:14%;border-radius:50%;border:1px dashed rgba(95,240,255,.14)}
.joy .tick{position:absolute;width:0;height:0;border:5px solid transparent;opacity:.45}
.tick.n{top:8px;left:calc(50% - 5px);border-bottom-color:var(--a)}.tick.s{bottom:8px;left:calc(50% - 5px);border-top-color:var(--a)}
.tick.w{left:8px;top:calc(50% - 5px);border-right-color:var(--a)}.tick.e{right:8px;top:calc(50% - 5px);border-left-color:var(--a)}
.knob{position:absolute;left:50%;top:50%;width:34%;aspect-ratio:1;margin:-17% 0 0 -17%;border-radius:50%;
 background:radial-gradient(circle at 35% 30%,#bffaff 0,#5ff0ff 22%,#169bc4 70%,#0b5d78 100%);box-shadow:0 8px 20px rgba(0,0,0,.6),0 0 24px rgba(95,240,255,.45);transition:transform .18s cubic-bezier(.2,1.4,.4,1)}
.joy.active .knob{transition:none;box-shadow:0 8px 20px rgba(0,0,0,.6),0 0 40px rgba(95,240,255,.8)}
.side{display:flex;flex-direction:column;gap:7px;min-width:0}
.talkrow{display:flex;align-items:center;gap:10px}
.mic{position:relative;flex:none;width:clamp(64px,11dvh,96px);aspect-ratio:1;border-radius:50%;border:0;touch-action:none;color:#04161b;
 background:radial-gradient(circle at 35% 30%,#d6fdff,#5ff0ff 45%,#1aa6cf);box-shadow:0 10px 26px rgba(0,0,0,.55),0 0 26px rgba(95,240,255,.4);display:grid;place-items:center}
.mic svg{width:42%;height:42%}
.mic.held{background:radial-gradient(circle at 35% 30%,#ffd6de,#ff5a78 50%,#c3203f);color:#fff}
.mic.held:before,.mic.held:after{content:"";position:absolute;inset:-8px;border-radius:50%;border:2px solid rgba(255,90,120,.5);animation:ring 1.4s infinite}
.mic.held:after{animation-delay:.7s}@keyframes ring{from{transform:scale(.9);opacity:1}to{transform:scale(1.35);opacity:0}}
.tinfo{min-width:0;display:flex;flex-direction:column;gap:4px}
.state{display:inline-flex;align-items:center;gap:7px;padding:5px 10px;border-radius:99px;background:rgba(255,255,255,.05);border:1px solid var(--line);font-size:12px;font-weight:600;max-width:100%;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.state i{flex:none;width:8px;height:8px;border-radius:50%;background:#55606a}
.state.listen i{background:var(--red);box-shadow:0 0 10px var(--red);animation:pulse 1s infinite}.state.think i{background:var(--warn);animation:pulse .6s infinite}
.state.speak i{background:var(--a);box-shadow:0 0 10px var(--a);animation:pulse .8s infinite}.state.ready i{background:var(--ok)}.state.err i{background:var(--red)}
@keyframes pulse{50%{opacity:.35}}
.cap{color:var(--m);font-size:11.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.btns{display:grid;grid-template-columns:1.3fr 1fr 1fr;gap:6px}
.b{padding:9px 4px;border-radius:12px;border:1px solid var(--line);background:var(--card);color:var(--t);font:600 12.5px inherit;font-family:inherit;white-space:nowrap}
.b.stop{background:rgba(255,77,106,.14);color:var(--red);border-color:rgba(255,77,106,.4);letter-spacing:1px}
.b.act{color:var(--a);border-color:rgba(95,240,255,.45);background:rgba(95,240,255,.1)}
.row{display:flex;align-items:center;gap:8px;color:var(--m);font-size:12px}
.row b{color:var(--t);font-weight:600;min-width:30px;text-align:right}
input[type=range]{flex:1;min-width:0;-webkit-appearance:none;appearance:none;height:5px;border-radius:9px;background:linear-gradient(90deg,var(--a2),var(--a)) 0/var(--p,50%) 100% no-repeat,rgba(255,255,255,.1)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:#fff;box-shadow:0 2px 8px rgba(0,0,0,.5),0 0 0 4px rgba(95,240,255,.25)}
input[type=range]::-moz-range-thumb{width:20px;height:20px;border:0;border-radius:50%;background:#fff}
.seg3{display:grid;grid-template-columns:repeat(3,1fr);background:rgba(255,255,255,.05);border:1px solid var(--line);border-radius:10px;padding:2px;gap:2px;flex:1}
.seg3 button{padding:4px 0;border:0;border-radius:8px;background:none;color:var(--m);font:600 11.5px inherit;font-family:inherit}
.seg3 button.act{background:#1c2a31;color:var(--t);box-shadow:inset 0 0 0 1px rgba(95,240,255,.35)}
.alarmbar{display:none;position:fixed;left:10px;right:10px;top:max(8px,env(safe-area-inset-top));z-index:20;padding:12px;border-radius:16px;border:0;
 background:linear-gradient(135deg,#ff4d6a,#ff8a4d);color:#fff;font:800 16px inherit;font-family:inherit;letter-spacing:.5px;box-shadow:0 10px 40px rgba(255,77,106,.55);animation:glowa 1.2s infinite}
@keyframes glowa{50%{box-shadow:0 0 0 6px rgba(255,77,106,.35),0 10px 40px rgba(255,77,106,.8)}}
.alarmbar small{display:block;font-weight:600;font-size:12px;opacity:.9}
.alarmbar.show{display:block}
/* tall phones: talk + buttons, then a big joystick under them */
@media (max-aspect-ratio:2/3){
 .ctl{grid-template-columns:1fr;justify-items:center;gap:12px}.side{width:100%;order:-1}
 .joy{width:clamp(130px,min(64vw,calc(100dvh - 100vw * 3 / 4 - 290px)),250px)}
}
/* landscape phones, tablets and desktop: camera left, controls right */
@media (orientation:landscape) and (min-aspect-ratio:5/4){
 .app{grid-template-columns:minmax(0,1fr) auto;grid-template-rows:auto minmax(0,1fr);height:100dvh}
 header{grid-column:1/-1}
 .camwrap{container-type:size;height:100%}.cam{width:min(100cqw,133.33cqh)}
 .ctl{grid-template-columns:1fr;grid-auto-rows:min-content;align-content:center;justify-items:center;width:clamp(230px,30vw,330px)}
 .joy{width:clamp(110px,min(26vw,38dvh),220px)}.side{width:100%}.mic{width:clamp(56px,13dvh,92px)}
}
@media (max-height:520px) and (orientation:landscape){.ctl{grid-template-columns:auto minmax(0,1fr);width:clamp(300px,42vw,420px)}.joy{width:clamp(100px,42dvh,170px)}}
/* ---------- sheets (scroll inside, the main screen never does) ---------- */
.sheet{position:fixed;inset:0;z-index:10;background:rgba(7,9,12,.97);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);overflow-y:auto;-webkit-overflow-scrolling:touch;
 transform:translateY(102%);transition:transform .25s ease;padding:max(10px,env(safe-area-inset-top)) 14px calc(20px + env(safe-area-inset-bottom))}
.sheet.open{transform:none}.sheet>div{max-width:560px;margin:0 auto}
.sh{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;position:sticky;top:0;padding:4px 0;background:rgba(7,9,12,.97);z-index:1}
.sh h2{font-size:18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:14px;margin-bottom:12px}
.card h3{font-size:12px;letter-spacing:1.2px;text-transform:uppercase;color:var(--m);margin-bottom:10px;font-weight:600}
.btn{padding:10px 14px;border-radius:13px;border:1px solid rgba(95,240,255,.35);background:rgba(95,240,255,.08);color:var(--a);font:600 14px inherit;font-family:inherit}
.btn.full{width:100%}.btn.dim{border-color:var(--line);background:var(--card);color:var(--t)}.btn.red{border-color:rgba(255,77,106,.35);background:rgba(255,77,106,.1);color:var(--red)}
.list{margin-bottom:12px;border-radius:var(--r);overflow:hidden;border:1px solid var(--line);background:var(--card)}
.sw{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 14px;border-top:1px solid var(--line)}
.sw:first-child{border-top:0}.sw span small{display:block;color:var(--m);font-size:12px;margin-top:2px}
.sw input{display:none}.sw i{flex:none;width:44px;height:26px;border-radius:99px;background:rgba(255,255,255,.14);position:relative;transition:.2s}
.sw i:after{content:"";position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;background:#fff;transition:.2s;box-shadow:0 2px 6px rgba(0,0,0,.4)}
.sw input:checked+i{background:linear-gradient(90deg,var(--a2),var(--a))}.sw input:checked+i:after{transform:translateX(18px)}
.compose{display:flex;gap:8px;align-items:center;background:rgba(255,255,255,.05);border:1px solid var(--line);border-radius:16px;padding:5px 5px 5px 12px}
.compose input,.field{flex:1;min-width:0;background:none;border:0;outline:0;color:var(--t);font:16px inherit;font-family:inherit;-webkit-user-select:text;user-select:text}
.field{display:block;width:100%;padding:11px 12px;margin-bottom:8px;border-radius:13px;background:rgba(255,255,255,.05);border:1px solid var(--line);color-scheme:dark}
select.field{-webkit-appearance:none;appearance:none}
.send{width:40px;height:40px;border-radius:13px;border:0;background:linear-gradient(135deg,var(--a),var(--a2));color:#04161b;display:grid;place-items:center;flex:none}
.send svg{width:19px;height:19px}
.emo{display:grid;grid-template-columns:repeat(8,1fr);gap:4px;margin-top:10px}
.emo button{font-size:23px;padding:5px 0;border:0;border-radius:12px;background:none}.emo button:active{background:rgba(255,255,255,.08)}
.sent{height:18px;margin-top:8px;color:var(--ok);font-size:12.5px;text-align:center;opacity:0;transition:.3s}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:12px}
.grid div{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:9px;font-size:11.5px;color:var(--m)}
.grid b{display:block;color:var(--t);font-size:13.5px;margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.grid .w2{grid-column:span 2}.grid .w3{grid-column:span 3}
.net{display:flex;align-items:center;gap:10px;padding:11px 0;border-top:1px solid var(--line)}.net:first-child{border-top:0}
.net .n{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.net small{color:var(--m);display:block;font-size:12px}
.net button{border:0;background:none;color:var(--m);font-size:17px;padding:4px 8px}.net .star.on{color:var(--warn)}
.big{font-size:19px;font-weight:700}.mut{color:var(--m);font-size:12.5px}.gap{height:10px}
.results button{display:block;width:100%;text-align:left;padding:10px 12px;margin-top:6px;border-radius:12px;border:1px solid var(--line);background:rgba(255,255,255,.04);color:var(--t);font:14px inherit;font-family:inherit}
.al{display:flex;align-items:center;gap:12px;padding:12px 0;border-top:1px solid var(--line)}.al:first-child{border-top:0}
.al .t{font-size:24px;font-weight:700;font-variant-numeric:tabular-nums;min-width:78px}.al.off .t{color:var(--m)}
.al .d{flex:1;min-width:0}.al .d small{display:block;color:var(--m);font-size:12px}
.al .x{border:0;background:none;color:var(--m);font-size:16px;padding:6px}
.al label.sw{border:0;padding:0}
.days{display:grid;grid-template-columns:repeat(7,1fr);gap:5px;margin-bottom:8px}
.days button{padding:9px 0;border-radius:11px;border:1px solid var(--line);background:var(--card);color:var(--m);font:700 13px inherit;font-family:inherit}
.days button.on{color:#04161b;background:var(--a);border-color:var(--a)}
.quick{display:flex;gap:6px;margin-bottom:8px}.quick button{flex:1;padding:7px 0;border-radius:10px;border:1px solid var(--line);background:none;color:var(--m);font:600 12px inherit;font-family:inherit}
</style></head><body>
<button class="alarmbar" id="alarmbar">⏰ STOP ALARM<small id="alarmsub">Alarm ringing</small></button>
<div class="app">
<header><div class="logo"><div class="eyes"><i></i><i></i></div>Chitu</div>
<div class="pill" id="conn"><span class="dot"></span><span id="connt">Connecting</span></div>
<button class="hb" id="oAlarm" title="Alarms"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="13" r="8"/><path d="M12 9v4l2.5 2M5 3L2 6M19 3l3 3"/></svg></button>
<button class="hb" id="oMsg" title="Message"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg></button>
<button class="hb" id="oPet" title="Chitu"><svg viewBox="0 0 24 24" fill="currentColor"><circle cx="6" cy="9" r="2.2"/><circle cx="10" cy="5.5" r="2.2"/><circle cx="14" cy="5.5" r="2.2"/><circle cx="18" cy="9" r="2.2"/><path d="M12 11c-3 0-6 4.5-6 7a3 3 0 0 0 3 3c1.2 0 2-.6 3-.6s1.8.6 3 .6a3 3 0 0 0 3-3c0-2.5-3-7-6-7z"/></svg></button>
<button class="hb" id="oSetup" title="Setup"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M2 12h3M19 12h3M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1"/></svg></button></header>

<div class="camwrap"><div class="cam"><img id="cam" alt="">
<div class="msg" id="cammsg"><button class="play" id="camPlay"><svg viewBox="0 0 24 24" fill="currentColor"><path d="M7 4l13 8-13 8z"/></svg></button><span id="cammsgt">Camera is off. Tap to watch live.</span></div>
<div class="tools">
<button class="ib off" id="cOn" title="Live view"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M23 7l-7 5 7 5V7z"/><rect x="1" y="5" width="15" height="14" rx="2"/></svg></button>
<button class="ib" id="cFlip" title="Flip"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v18M7 8l5-5 5 5M7 16l5 5 5-5"/></svg></button>
<button class="ib" id="cMirror" title="Mirror"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12h18M8 7l-5 5 5 5M16 7l5 5-5 5"/></svg></button></div>
<div class="hud"><span class="chip" id="dist">— cm</span><span class="chip" id="stchip">idle</span><span class="chip" id="camchip">camera off</span></div></div></div>

<div class="ctl">
<div class="joy" id="joy"><span class="tick n"></span><span class="tick s"></span><span class="tick w"></span><span class="tick e"></span><div class="knob" id="knob"></div></div>
<div class="side">
<div class="talkrow"><button class="mic" id="talk"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 10a7 7 0 0 0 14 0M12 17v5M8 22h8"/></svg></button>
<div class="tinfo"><span class="state" id="tstate"><i></i><span id="tstatet">Ready</span></span><div class="cap" id="talkcap">Hold to talk</div></div></div>
<div class="btns"><button class="b stop" id="stop">STOP</button><button class="b" id="hush">Hush</button><button class="b" id="roam">Roam</button></div>
<div class="row">Speed<div class="seg3" id="speedseg"><button data-v="50">Slow</button><button data-v="75" class="act">Normal</button><button data-v="100">Fast</button></div></div>
<div class="row">Vol<input id="vol" type="range" min="0" max="100" value="100"><b id="volv">100</b></div>
</div></div></div>

<!-- ======================= sheets ======================= -->
<section class="sheet" id="s-alarm"><div>
<div class="sh"><h2>Alarms</h2><button class="hb" data-close>✕</button></div>
<button class="btn red full" id="alStop" style="display:none;margin-bottom:12px">⏰ STOP ALARM</button>
<div class="card"><div class="mut" id="alNow">—</div><div class="gap"></div><div id="alList"></div>
<div class="gap"></div><button class="btn full" id="alNew">+ New alarm</button></div>
<div class="card" id="alForm" style="display:none"><h3 id="alFormT">New alarm</h3>
<input class="field" id="alTime" type="time" value="07:00">
<div class="quick"><button data-q="0">Once</button><button data-q="127">Daily</button><button data-q="62">Weekdays</button><button data-q="65">Weekends</button></div>
<div class="days" id="alDays"></div>
<input class="field" id="alLabel" maxlength="30" placeholder="Label (optional)">
<div class="row" style="gap:8px"><button class="btn full" id="alSave">Save</button><button class="btn dim full" id="alCancel">Cancel</button></div>
<div class="mut" id="alMsg" style="margin-top:8px"></div></div>
<div class="mut">When an alarm goes off Chitu rings, talks, wiggles on the spot and shows wide-awake eyes until you stop it: say <b>"Chitu … stop alarm"</b>, press STOP ALARM here, or press the BOOT button on the robot.</div>
</div></section>

<section class="sheet" id="s-msg"><div>
<div class="sh"><h2>Message</h2><button class="hb" data-close>✕</button></div>
<div class="card"><div class="compose"><input id="text" maxlength="400" placeholder="Message for Chitu's screen" enterkeyhint="send"><button class="send" id="send"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M22 2L11 13M22 2l-7 20-4-9-9-4z"/></svg></button></div>
<div class="emo" id="emo"></div><div class="sent" id="sent">Sent to Chitu ✓</div></div>
<div class="list"><label class="sw"><span>Read it out loud<small>Chitu reads your message word for word (needs Wi-Fi)</small></span><input type="checkbox" id="sayit"><i></i></label></div>
</div></section>

<section class="sheet" id="s-pet"><div>
<div class="sh"><h2>Chitu</h2><button class="hb" data-close>✕</button></div>
<div class="card"><h3>Mood</h3><div class="big" id="mood">—</div><div class="mut" id="moodsub"></div></div>
<div class="list">
<label class="sw"><span>Chatty<small>Sometimes talks on its own (never 22:00-08:00)</small></span><input type="checkbox" id="tChat" checked><i></i></label>
<label class="sw"><span>Little moves<small>Wiggles and turns on the spot by itself</small></span><input type="checkbox" id="tMoves" checked><i></i></label></div>
<div class="card"><h3>Voice</h3><select class="field" id="voice" style="margin:0"><option value="0">Natural</option><option value="1">Cute (higher)</option><option value="2">Tiny (very high)</option><option value="3">Deep (lower)</option><option value="4">Robot</option></select></div>
<div class="list">
<label class="sw"><span>Chitu's voice on phone<small>Hear Chitu's replies here</small></span><input type="checkbox" id="tPhone" checked><i></i></label>
<label class="sw"><span>Robot speaker<small>Also play on Chitu</small></span><input type="checkbox" id="tSpk" checked><i></i></label>
<label class="sw"><span>Hear the room<small>Listen through Chitu's mic</small></span><input type="checkbox" id="tRoom"><i></i></label>
<label class="sw"><span>Phone mic always on<small>Say the wake word to your phone</small></span><input type="checkbox" id="tMic"><i></i></label></div>
<div class="card" id="micnote" style="display:none;color:#ffd28a"></div>
<div class="grid"><div>State<b id="state">—</b></div><div>Wi-Fi<b id="rssi">—</b></div><div>Mic<b id="mic">—</b></div>
<div class="w3">Wake word<b id="wake">—</b></div></div>
</div></section>

<section class="sheet" id="s-setup"><div>
<div class="sh"><h2>Setup</h2><button class="hb" data-close>✕</button></div>
<div class="card"><h3>Wi-Fi</h3><div class="big" id="wnow">—</div><div class="mut" id="wsub"></div>
<div class="gap"></div><div id="wlist"></div><div class="gap"></div>
<input class="field" id="wssid" placeholder="Network name" autocomplete="off" autocapitalize="off">
<input class="field" id="wpw" type="password" placeholder="Password" autocomplete="new-password">
<button class="btn full" id="wadd">Save network</button><div class="mut" id="wmsg" style="margin-top:8px"></div>
<div class="gap"></div><div class="row" style="gap:8px"><select class="field" id="woffmin" style="margin:0;flex:1"><option value="5">Off for 5 min</option><option value="30">Off for 30 min</option><option value="0">Off until BOOT button</option></select>
<button class="btn red" id="woff">Wi-Fi off</button></div>
<div class="mut" style="margin-top:8px">Turning Wi-Fi off closes this page. Chitu reconnects by itself after the time, or when you press its BOOT button.</div></div>
<div class="card"><h3>Place and time</h3><div class="big" id="lcity">—</div><div class="mut" id="lsub"></div>
<div class="gap"></div><div class="compose"><input id="lq" placeholder="Search your city" enterkeyhint="search"><button class="send" id="lgo"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/></svg></button></div>
<div class="results" id="lres"></div><div class="gap"></div>
<div class="row" style="margin-bottom:8px">Time zone</div><select class="field" id="ltz"></select>
<button class="btn dim full" id="lauto">Detect city automatically</button></div>
<div class="list"><label class="sw"><span>Camera allowed<small>Off = no live view and no photos for the AI</small></span><input type="checkbox" id="tCam" checked><i></i></label></div>
<div class="grid"><div>Camera<b id="hcam">—</b></div><div>Free RAM<b id="hram">—</b></div><div>PSRAM<b id="hps">—</b></div>
<div>Uptime<b id="hup">—</b></div><div>Wi-Fi drops<b id="hdrop">—</b></div><div>Clock<b id="hntp">—</b></div>
<div class="w3">Distance sensor<b id="htof">—</b></div></div>
</div></section>

<script>
const $=id=>document.getElementById(id);
const cmd=q=>fetch('/cmd?'+q).then(r=>r.json()).catch(()=>({ok:false}));
const buzz=ms=>{try{navigator.vibrate&&navigator.vibrate(ms)}catch(e){}};
const esc=s=>String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function fill(r){r.style.setProperty('--p',((r.value-r.min)/(r.max-r.min)*100)+'%')}
// ---------- sheets ----------
let sheet=null;
function openSheet(id){AC();document.querySelectorAll('.sheet').forEach(s=>s.classList.toggle('open',s.id===id));sheet=id;if(id==='s-setup')setup();if(id==='s-alarm')alarms()}
function closeSheets(){document.querySelectorAll('.sheet').forEach(s=>s.classList.remove('open'));sheet=null}
$('oAlarm').onclick=()=>openSheet('s-alarm');$('oMsg').onclick=()=>openSheet('s-msg');$('oPet').onclick=()=>openSheet('s-pet');$('oSetup').onclick=()=>openSheet('s-setup');
document.querySelectorAll('[data-close]').forEach(b=>b.onclick=closeSheets);
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
 try{const st=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true,noiseSuppression:true,autoGainControl:true,channelCount:1}});
  const src=c.createMediaStreamSource(st),p=c.createScriptProcessor(2048,1,1);
  p.onaudioprocess=e=>{if(!(holding||micAlways)||!ws||ws.readyState!==1)return;const d=e.inputBuffer.getChannelData(0),step=c.sampleRate/16000,out=[];
   for(let i=0;i<d.length;i++){dsSum+=d[i];dsCnt++;if(++dsAcc>=step){dsAcc-=step;out.push(dsSum/dsCnt);dsSum=0;dsCnt=0}}
   const buf=new DataView(new ArrayBuffer(1+out.length*2));buf.setUint8(0,1);
   for(let i=0;i<out.length;i++){let v=Math.max(-1,Math.min(1,out[i]*1.4));buf.setInt16(1+i*2,v*32767,true)}ws.send(buf.buffer)};
  const z=c.createGain();z.gain.value=0;src.connect(p);p.connect(z);z.connect(c.destination);mic={st,p};note('');return true}
 catch(e){note('Microphone blocked: allow microphone access for this page in your browser settings, then reload.');return false}}
function note(t){$('micnote').textContent=t;$('micnote').style.display=t?'block':'none';if(t)$('talkcap').textContent=t}
const talk=$('talk');
talk.addEventListener('pointerdown',async e=>{e.preventDefault();talk.setPointerCapture(e.pointerId);
 if(!ws||ws.readyState!==1){$('talkcap').textContent='Not connected yet - try again';return}
 if(!await micStart())return;holding=true;buzz(15);talk.classList.add('held');$('talkcap').textContent='Talk now… release to send';cmd('a=talk&on=1');fast=6});
['pointerup','pointercancel','lostpointercapture'].forEach(ev=>talk.addEventListener(ev,()=>{if(!holding)return;talk.classList.remove('held');$('talkcap').textContent='Hold to talk';
 setTimeout(()=>{holding=false;cmd('a=talk&on=0')},350)}));
$('hush').onclick=()=>{cmd('a=hush');buzz(30)};
function sw(id,key,cb){const el=$(id);el.onchange=async()=>{AC();if(cb&&!(await cb(el.checked))){el.checked=!el.checked;return}cmd('a='+key+'&on='+(el.checked?1:0))};return el}
const tPhone=sw('tPhone','audio'),tSpk=sw('tSpk','spk'),tRoom=sw('tRoom','room');
sw('tMic','mic',async on=>{if(on&&!await micStart())return false;micAlways=on;return true});
const tChat=sw('tChat','chatty'),tMoves=sw('tMoves','moves'),tCam=sw('tCam','cam_allow');
// ---------- camera + audio + drive stream ----------
let ws=null,camUrl=null,camOn=false;const cam=$('cam');
const wsSend=t=>{if(ws&&ws.readyState===1){ws.send(t);return true}return false};
function camMsg(t,mode){$('cammsg').style.display=t?'flex':'none';$('cammsgt').textContent=t||'';
 $('camPlay').style.display=mode==='play'?'grid':'none';let sp=$('cammsg').querySelector('.spin');if(mode==='spin'&&!sp){sp=document.createElement('div');sp.className='spin';$('cammsg').prepend(sp)}if(sp)sp.style.display=mode==='spin'?'block':'none'}
function setCam(on){camOn=on;$('cOn').classList.toggle('off',!on);cmd('a=video&on='+(on?1:0));if(on){camMsg('Starting camera','spin')}else{cam.removeAttribute('src');camMsg('Camera is off. Tap to watch live.','play')}}
function connect(){ws=new WebSocket('wss://'+location.host+'/ws');ws.binaryType='arraybuffer';
 ws.onopen=()=>{ws.send('p');cmd('a=audio&on='+(tPhone.checked?1:0));cmd('a=spk&on='+(tSpk.checked?1:0));cmd('a=room&on='+(tRoom.checked?1:0));if(camOn)setCam(true)};
 ws.onmessage=e=>{if(typeof e.data==='string')return;const v=new DataView(e.data),t=v.getUint8(0);
  if(t===0x10){if(!camOn)return;const u=URL.createObjectURL(new Blob([new Uint8Array(e.data,1)],{type:'image/jpeg'}));cam.onload=()=>{if(camUrl)URL.revokeObjectURL(camUrl);camUrl=u};cam.src=u;camMsg('')}
  else if(t===0x20)play(v,rates.or,players.v);else if(t===0x21)play(v,rates.ir,players.r)};
 ws.onclose=()=>{if(camOn)camMsg('Reconnecting','spin');setTimeout(connect,2000)}}
connect();setInterval(()=>wsSend('p'),1000);
$('cOn').onclick=()=>setCam(!camOn);$('camPlay').onclick=()=>{AC();setCam(true)};
let camV=0,camH=0;$('cFlip').onclick=()=>{camV^=1;cmd('a=cam&v='+camV)};$('cMirror').onclick=()=>{camH^=1;cmd('a=cam&h='+camH)};
// ---------- joystick (sent over the WebSocket; HTTP only as a fallback) ----------
const joy=$('joy'),knob=$('knob');let jx=0,jy=0,jActive=false,jTimer=null,lastSector=-1,maxSpeed=75;
function jSend(){const x=Math.round(jx*100),y=Math.round(jy*100);if(!wsSend('j '+x+' '+y+' '+maxSpeed))cmd('a=joy&x='+x+'&y='+y+'&s='+maxSpeed)}
function jMove(e){const r=joy.getBoundingClientRect(),R=r.width/2,max=R*0.62;let dx=e.clientX-(r.left+R),dy=e.clientY-(r.top+R);
 const d=Math.hypot(dx,dy);if(d>max){dx*=max/d;dy*=max/d}knob.style.transform='translate('+dx+'px,'+dy+'px)';jx=dx/max;jy=-dy/max;
 const mag=Math.hypot(jx,jy),sec=mag<0.25?-1:Math.round((Math.atan2(jy,jx)/(Math.PI/4)+8))%8;if(sec!==lastSector){lastSector=sec;buzz(8);jSend()}}
joy.addEventListener('pointerdown',e=>{e.preventDefault();joy.setPointerCapture(e.pointerId);jActive=true;joy.classList.add('active');roamOn=false;$('roam').classList.remove('act');jMove(e);jSend();jTimer=setInterval(jSend,150)});
joy.addEventListener('pointermove',e=>{if(jActive)jMove(e)});
function stopNow(){wsSend('x');cmd('a=stop')}
function jEnd(){if(!jActive)return;jActive=false;joy.classList.remove('active');clearInterval(jTimer);jx=jy=0;lastSector=-1;knob.style.transform='';stopNow()}
['pointerup','pointercancel','lostpointercapture'].forEach(ev=>joy.addEventListener(ev,jEnd));
let roamOn=false;
$('stop').onclick=()=>{jEnd();roamOn=false;$('roam').classList.remove('act');stopNow();buzz(30)};
$('roam').onclick=async()=>{roamOn=!roamOn;$('roam').classList.toggle('act',roamOn);const j=await cmd('a=roam&on='+(roamOn?1:0));
 if(roamOn&&!j.ok){roamOn=false;$('roam').classList.remove('act');$('talkcap').textContent='Roaming needs the distance sensor'}};
document.querySelectorAll('#speedseg button').forEach(b=>b.onclick=()=>{maxSpeed=+b.dataset.v;document.querySelectorAll('#speedseg button').forEach(x=>x.classList.toggle('act',x===b))});
const vol=$('vol');fill(vol);
vol.oninput=()=>{$('volv').textContent=vol.value;fill(vol)};vol.onchange=()=>cmd('a=volume&v='+vol.value);
$('voice').onchange=()=>cmd('a=voice&p='+$('voice').value);
document.addEventListener('visibilitychange',()=>{if(document.hidden){jEnd();if(holding){holding=false;cmd('a=talk&on=0')}}});
// ---------- messages ----------
'😀😂😍🥰😘😎🤔😴😢😡😱🤖👍👋🙏❤🔥🎉⭐🍕🐶🌙☕🎂'.match(/\p{Extended_Pictographic}/gu).forEach(c=>{const b=document.createElement('button');b.textContent=c;b.onclick=()=>{$('text').value+=c};$('emo').appendChild(b)});
function sendMsg(){let t=$('text').value.trim();if(!t)return;t=t.replace(/[️‍]|[\u{1F3FB}-\u{1F3FF}]/gu,'');
 fetch('/msg?say='+($('sayit').checked?1:0),{method:'POST',body:t}).then(r=>r.json()).then(j=>{$('sent').textContent=j.note||'Sent to Chitu ✓';$('text').value='';$('sent').style.opacity=1;buzz(12);setTimeout(()=>$('sent').style.opacity=0,2500)}).catch(()=>{})}
$('send').onclick=sendMsg;$('text').addEventListener('keydown',e=>{if(e.key==='Enter')sendMsg()});
// ---------- alarms ----------
const DN=['S','M','T','W','T','F','S'],DORDER=[1,2,3,4,5,6,0];let alDays=0,alEdit=null,alData=null;
DORDER.forEach(d=>{const b=document.createElement('button');b.textContent=DN[d];b.dataset.d=d;b.onclick=()=>{alDays^=1<<d;drawDays()};$('alDays').appendChild(b)});
function drawDays(){$('alDays').querySelectorAll('button').forEach(b=>b.classList.toggle('on',!!(alDays&(1<<b.dataset.d))))}
document.querySelectorAll('.quick button').forEach(b=>b.onclick=()=>{alDays=+b.dataset.q;drawDays()});
const pad=n=>String(n).padStart(2,'0');
function alForm(a){alEdit=a?a.id:null;$('alFormT').textContent=a?'Edit alarm':'New alarm';$('alTime').value=a?pad(a.h)+':'+pad(a.m):'07:00';
 alDays=a?a.days:0;drawDays();$('alLabel').value=a?a.label:'';$('alMsg').textContent='';$('alForm').style.display='block';$('alForm').scrollIntoView({behavior:'smooth'})}
$('alNew').onclick=()=>alForm(null);$('alCancel').onclick=()=>{$('alForm').style.display='none'};
async function alPost(p){try{return await (await fetch('/alarms',{method:'POST',body:new URLSearchParams(p)})).json()}catch(e){return{ok:false,error:'No connection'}}}
$('alSave').onclick=async()=>{const t=$('alTime').value.split(':');if(t.length<2){$('alMsg').textContent='Choose a time';return}
 const p={op:'save',h:+t[0],m:+t[1],days:alDays,label:$('alLabel').value,en:1};if(alEdit!==null)p.id=alEdit;
 const j=await alPost(p);if(j.ok){$('alForm').style.display='none';buzz(15);alarms()}else $('alMsg').textContent=j.error||'Could not save'};
async function alarms(){try{const d=await (await fetch('/alarms',{cache:'no-store'})).json();alData=d;
 $('alNow').textContent=d.time_ok?'Time on Chitu: '+d.now:'Chitu\'s clock is not set yet (needs internet) - alarms wait for it';
 $('alList').innerHTML=d.alarms.length?d.alarms.map(a=>'<div class="al'+(a.en?'':' off')+'" data-id="'+a.id+'"><div class="t">'+a.time+'</div><div class="d">'+esc(a.label||'Alarm')+'<small>'+esc(a.repeat)+(a.next?' · '+esc(a.next):'')+'</small></div><label class="sw"><input type="checkbox" '+(a.en?'checked':'')+'><i></i></label><button class="x" title="Delete">✕</button></div>').join(''):'<div class="mut">No alarms yet.</div>';
 $('alList').querySelectorAll('.al').forEach(row=>{const id=row.dataset.id,a=d.alarms.find(x=>String(x.id)===id);
  row.querySelector('input').onchange=async e=>{await alPost({op:'en',id,on:e.target.checked?1:0});alarms()};
  row.querySelector('.x').onclick=async e=>{const b=e.currentTarget;if(b.dataset.arm){await alPost({op:'del',id});alarms()}else{b.dataset.arm=1;b.textContent='Delete?'}};
  row.querySelector('.d').onclick=()=>alForm(a);row.querySelector('.t').onclick=()=>alForm(a)});
 $('alNew').disabled=d.alarms.length>=d.max}catch(e){}}
function stopAlarm(){cmd('a=alarm_stop');buzz(40);$('alarmbar').classList.remove('show');$('alStop').style.display='none'}
$('alarmbar').onclick=stopAlarm;$('alStop').onclick=stopAlarm;
// ---------- status ----------
const ST={idle:['ready','Ready'],connecting:['think','Connecting…'],listening:['listen','Listening…'],speaking:['speak','Chitu is talking'],
 activating:['think','Activating'],wifi_configuring:['err','Wi-Fi setup mode'],starting:['think','Starting'],upgrading:['think','Updating'],fatal_error:['err','Error']};
const MOOD={calm:'Calm and content',happy:'Happy',curious:'Curious',playful:'Playful',bored:'A bit bored',sleepy:'Sleepy',asleep:'Asleep',excited:'Excited',surprised:'Startled',cautious:'Cautious'};
let fails=0,volSet=false,synced=false,fast=0,tofTxt='—';
function distText(t){if(!t||t.st==='none')return['no sensor',''];if(t.st==='lost'||t.st==='starting')return['sensor '+t.st,' warn'];
 if(t.st==='timeout')return['sensor slow',' warn'];const mm=t.mm;const s=mm>=2000?'> 2 m':(mm>=1000?(mm/1000).toFixed(1)+' m':Math.round(mm/10)+' cm');
 return[s,t.blocked?' bad':(mm<t.stop_mm*2?' warn':'')]}
async function poll(){try{const r=await fetch('/status',{cache:'no-store'});const s=await r.json();fails=0;
 $('conn').className='pill on';$('connt').textContent=s.rssi+' dBm';rates.ir=s.ir;rates.or=s.or;camV=s.cam_v|0;camH=s.cam_h|0;
 const dt=distText(s.tof),d=$('dist');d.textContent=dt[0];d.className='chip'+dt[1];
 const t=s.tof||{};tofTxt=t.st==='none'?'not fitted':(t.st+' · raw '+t.raw+' mm · filtered '+t.mm+' mm'+(t.blocked?' · OBSTACLE':'')+' · stop < '+t.stop_mm+' mm');
 const st=s.m.roaming?'roaming':(s.m.dancing?'dancing':(s.m.moving?'moving':s.st));$('state').textContent=st;
 const blk=s.m.obstacle_stop,fault=s.m.path==='sensor_fault';$('stchip').textContent=blk?(fault?'sensor fault':'obstacle'):st;$('stchip').className='chip'+(blk?' bad':'');
 if(!jActive){roamOn=s.m.roaming;$('roam').classList.toggle('act',roamOn)}
 const ts=ST[s.st]||['think',s.st];$('tstate').className='state '+ts[0];$('tstatet').textContent=s.net_ok?ts[1]:'No internet';
 $('camchip').textContent='camera '+s.cam;$('camchip').className='chip'+(s.cam==='live'?' ok':(s.cam==='blocked'?' warn':''));
 if(s.cam==='blocked'&&camOn){camOn=false;$('cOn').classList.add('off');cam.removeAttribute('src');camMsg('Camera is switched off in Setup.','')}
 $('rssi').textContent=s.rssi+' dBm';$('mic').textContent=s.phone_mic?'phone':(s.mic+(s.mic_clip?' clip':''));$('wake').textContent=s.wake;
 const p=s.pet;$('mood').textContent=MOOD[p.mood]||p.mood;$('moodsub').textContent='Last: '+(p.last||'—')+(p.music?' · hears music':'')+' · idle '+Math.floor(p.idle_s/60)+' min';
 const al=s.alarm||{};$('alarmbar').classList.toggle('show',!!al.ringing);$('alStop').style.display=al.ringing?'block':'none';$('oAlarm').classList.toggle('ring',!!al.ringing);
 if(al.ringing){$('alarmsub').textContent=(al.label?al.label+' · ':'')+'ringing '+Math.floor(al.since_s/60)+':'+pad(al.since_s%60)+' · tap to stop';fast=Math.max(fast,1)}
 if(!volSet&&s.volume>=0){vol.value=s.volume;$('volv').textContent=s.volume;fill(vol);volSet=true}
 if(!synced){tChat.checked=p.chatty;tMoves.checked=p.moves;$('voice').value=s.voice;tCam.checked=s.cam!=='blocked';synced=true}
 if(sheet==='s-setup')$('htof').textContent=tofTxt;
}catch(e){if(++fails>2){$('conn').className='pill';$('connt').textContent='Offline'}}
setTimeout(poll,fast>0?(fast--,700):1500)}
poll();
// ---------- setup ----------
let zones=null,woffArm=0;
async function setup(){if(sheet!=='s-setup')return;try{const s=await (await fetch('/setup',{cache:'no-store'})).json();
 const n=s.net;$('wnow').textContent=n.state==='connected'?n.ssid:(n.state==='offline_by_owner'?'Wi-Fi off':'Searching…');
 $('wsub').textContent=n.state==='connected'?(n.ip+' · '+n.rssi+' dBm · connected '+n.up_min+' min'):(n.last_reason||'');
 $('wlist').innerHTML=n.saved.map(w=>'<div class="net"><button class="star'+(w.pref?' on':'')+'" data-i="'+w.i+'" title="Preferred">★</button><div class="n">'+esc(w.ssid)+'<small>'+(w.now?'connected':(w.pref?'preferred':'saved'))+'</small></div><button class="del" data-i="'+w.i+'" title="Forget">✕</button></div>').join('');
 $('wlist').querySelectorAll('.star').forEach(b=>b.onclick=async()=>{await cmd('a=wifi_pref&i='+b.dataset.i);setup()});
 $('wlist').querySelectorAll('.del').forEach(b=>b.onclick=async()=>{if(b.dataset.arm){await cmd('a=wifi_del&i='+b.dataset.i);setup()}else{b.dataset.arm=1;b.textContent='Forget?'}});
 const l=s.loc;$('lcity').textContent=l.city?(l.city+(l.country?', '+l.country:'')):'Unknown city';
 $('lsub').textContent=(l.manual?'Set by you':'Detected automatically (approximate)')+(l.time?' · '+l.time:'')+(l.temp!==undefined?' · '+l.temp+'°'+l.unit+' '+l.sky:'')+(l.ntp?'':' · clock not synced yet');
 if(!zones){zones=s.zones;$('ltz').innerHTML=zones.map(z=>'<option value="'+esc(z[1])+'">'+esc(z[0])+'</option>').join('')}
 $('ltz').value=zones.some(z=>z[1]===l.tz)&&l.tz_name.indexOf('/')<0?l.tz:'';
 $('hcam').textContent=s.cam;$('hram').textContent=s.heap_int+' KB';$('hps').textContent=s.heap_ps+' KB';$('hup').textContent=Math.floor(s.up_min/60)+'h '+(s.up_min%60)+'m';
 $('hdrop').textContent=n.drops+(n.restarts?' / '+n.restarts+' restarts':'');$('hntp').textContent=l.ntp?'synced':'not yet';$('htof').textContent=tofTxt;
}catch(e){}setTimeout(setup,5000)}
$('wadd').onclick=async()=>{const b=new URLSearchParams({ssid:$('wssid').value,pw:$('wpw').value});$('wmsg').textContent='Saving…';
 try{const j=await (await fetch('/wifi',{method:'POST',body:b})).json();$('wmsg').textContent=j.ok?'Saved. Chitu uses it when the preferred network is not around.':j.error;if(j.ok){$('wssid').value='';$('wpw').value=''}}catch(e){$('wmsg').textContent='Could not save'}setup()};
$('woff').onclick=async()=>{if(Date.now()-woffArm>4000){woffArm=Date.now();$('woff').textContent='Tap again';setTimeout(()=>$('woff').textContent='Wi-Fi off',4000);return}
 await cmd('a=wifi_off&min='+$('woffmin').value);$('woff').textContent='Turning off…'};
async function citySearch(){const q=$('lq').value.trim();if(!q)return;$('lres').innerHTML='<div class="mut" style="margin-top:8px">Searching…</div>';
 try{const r=await (await fetch('/city?q='+encodeURIComponent(q))).json();
  $('lres').innerHTML=r.length?r.map((c,i)=>'<button data-i="'+i+'">'+esc(c.name)+'<span class="mut"> · '+esc([c.region,c.country].filter(Boolean).join(', '))+'</span></button>').join(''):'<div class="mut" style="margin-top:8px">No city found</div>';
  $('lres').querySelectorAll('button').forEach(b=>b.onclick=async()=>{const c=r[b.dataset.i];await fetch('/city',{method:'POST',body:new URLSearchParams({name:c.name,region:c.region,country:c.country,lat:c.lat,lon:c.lon,tz:c.tz})});$('lres').innerHTML='';$('lq').value='';setTimeout(setup,800)})}
 catch(e){$('lres').innerHTML='<div class="mut">Search failed</div>'}}
$('lgo').onclick=citySearch;$('lq').addEventListener('keydown',e=>{if(e.key==='Enter')citySearch()});
$('ltz').onchange=()=>{const z=zones.find(x=>x[1]===$('ltz').value);cmd('a=tz&p='+encodeURIComponent($('ltz').value)+'&l='+encodeURIComponent(z?z[0]:''));setTimeout(setup,600)};
$('lauto').onclick=async()=>{await cmd('a=loc_auto');$('lsub').textContent='Detecting…';setTimeout(setup,4000)};
</script></body></html>)HTML";

}  // namespace web_page
