/**
 * chat_page.h - 聊天应用嵌入式 HTML 前端页面
 *
 * 本文件包含聊天应用的完整前端 HTML/CSS/JS 代码，以 C++ 原始字符串字面量
 * 的形式嵌入。页面功能包括：
 *   - 用户登录/注册界面（auth-overlay）
 *   - 聊天主界面：左侧边栏（用户信息 + 聊天室列表）+ 右侧聊天区
 *   - WebSocket 实时通信：自动连接、断线重连、消息收发
 *   - 响应式布局：适配移动端窄屏
 *
 * UI 风格参考微信/企业微信，配色以绿色 (#07c160) 为主色调。
 */
#pragma once

inline const char* HTML_PAGE = R"html(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Coroutine Chat</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Hiragino Sans GB','Microsoft YaHei',sans-serif;background:#f5f5f5;color:#333;min-height:100vh}

/* ===== 登录页 ===== */
.auth-overlay{position:fixed;top:0;left:0;right:0;bottom:0;background:#f0f2f5;display:flex;justify-content:center;align-items:center;z-index:100}
.auth-card{width:380px;background:#fff;border-radius:8px;padding:40px 36px 32px;box-shadow:0 2px 12px rgba(0,0,0,.08)}
.auth-logo{text-align:center;margin-bottom:28px}
.auth-logo .icon{width:56px;height:56px;border-radius:12px;background:#07c160;display:inline-flex;align-items:center;justify-content:center;margin-bottom:12px}
.auth-logo .icon svg{width:30px;height:30px;fill:#fff}
.auth-logo h1{font-size:22px;font-weight:600;color:#1a1a1a}
.auth-logo p{font-size:13px;color:#999;margin-top:4px}
.auth-field{margin-bottom:16px}
.auth-field label{display:block;font-size:13px;color:#666;margin-bottom:6px}
.auth-field input{width:100%;padding:10px 14px;border:1px solid #dcdfe6;border-radius:6px;font-size:14px;outline:none;transition:border-color .2s;background:#fafafa}
.auth-field input:focus{border-color:#07c160;background:#fff}
.auth-btn{width:100%;padding:11px;border:none;border-radius:6px;font-size:15px;font-weight:500;cursor:pointer;transition:all .15s}
.auth-btn-primary{background:#07c160;color:#fff;margin-bottom:10px}
.auth-btn-primary:hover{background:#06ad56}
.auth-btn-secondary{background:#fff;color:#07c160;border:1px solid #07c160}
.auth-btn-secondary:hover{background:#f0faf4}
.auth-msg{padding:8px 12px;border-radius:6px;margin-top:14px;font-size:13px;display:none}
.auth-msg.ok{background:#f0f9eb;color:#67c23a;border:1px solid #c2e7b0;display:block}
.auth-msg.err{background:#fef0f0;color:#f56c6c;border:1px solid #fbc4c4;display:block}

/* ===== 聊天主界面 ===== */
.app{display:none;height:100vh;width:100vw}
.app-layout{display:flex;height:100%;max-width:1200px;margin:0 auto;box-shadow:0 0 20px rgba(0,0,0,.06)}

/* 左侧边栏 */
.sidebar{width:260px;background:#2e2e2e;display:flex;flex-direction:column;flex-shrink:0}
.sidebar-header{padding:16px 20px;display:flex;align-items:center;gap:12px;border-bottom:1px solid #3a3a3a}
.sidebar-avatar{width:40px;height:40px;border-radius:6px;display:flex;align-items:center;justify-content:center;font-size:18px;font-weight:600;color:#fff;flex-shrink:0}
.sidebar-info{flex:1;overflow:hidden}
.sidebar-info .name{font-size:14px;color:#e0e0e0;font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.sidebar-info .status{font-size:11px;color:#8a8a8a;margin-top:2px}
.sidebar-list{flex:1;overflow-y:auto;padding:8px 0}
.sidebar-item{padding:12px 20px;display:flex;align-items:center;gap:12px;cursor:pointer;transition:background .15s}
.sidebar-item:hover{background:#383838}
.sidebar-item.active{background:#383838}
.sidebar-item .room-icon{width:40px;height:40px;border-radius:6px;background:#07c160;display:flex;align-items:center;justify-content:center;flex-shrink:0}
.sidebar-item .room-icon svg{width:22px;height:22px;fill:#fff}
.sidebar-item .room-info{flex:1;overflow:hidden}
.sidebar-item .room-name{font-size:14px;color:#e0e0e0;font-weight:500}
.sidebar-item .room-desc{font-size:12px;color:#8a8a8a;margin-top:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.sidebar-bottom{padding:12px 20px;border-top:1px solid #3a3a3a}
.sidebar-bottom button{width:100%;padding:8px;background:transparent;border:1px solid #555;border-radius:6px;color:#aaa;font-size:13px;cursor:pointer;transition:all .15s}
.sidebar-bottom button:hover{background:#383838;color:#e0e0e0}

/* 右侧聊天区 */
.chat-main{flex:1;display:flex;flex-direction:column;background:#f0f0f0;min-width:0}
.chat-topbar{height:56px;background:#f5f5f5;border-bottom:1px solid #e0e0e0;padding:0 20px;display:flex;align-items:center;justify-content:space-between;flex-shrink:0}
.chat-topbar h2{font-size:16px;font-weight:500;color:#333}
.chat-topbar .online{font-size:12px;color:#999;margin-left:10px}
.chat-topbar-right{display:flex;align-items:center;gap:8px}
.topbar-btn{background:none;border:none;cursor:pointer;padding:6px;border-radius:4px;transition:background .15s}
.topbar-btn:hover{background:#e5e5e5}
.topbar-btn svg{width:20px;height:20px;fill:#666}

/* 消息区 */
#messages{flex:1;overflow-y:auto;padding:16px 20px;background:#f0f0f0}
#messages::-webkit-scrollbar{width:6px}
#messages::-webkit-scrollbar-track{background:transparent}
#messages::-webkit-scrollbar-thumb{background:#ccc;border-radius:3px}
.msg-row{display:flex;margin-bottom:16px;align-items:flex-start;gap:8px}
.msg-row.me{flex-direction:row-reverse}
.msg-avatar{width:36px;height:36px;border-radius:4px;display:flex;align-items:center;justify-content:center;font-size:14px;font-weight:600;color:#fff;flex-shrink:0}
.msg-body{max-width:60%;display:flex;flex-direction:column}
.msg-row.me .msg-body{align-items:flex-end}
.msg-name{font-size:12px;color:#999;margin-bottom:3px}
.msg-bubble{padding:10px 14px;border-radius:4px;font-size:14px;line-height:1.6;word-break:break-word;position:relative}
.msg-row:not(.me) .msg-bubble{background:#fff;color:#333;border-top-left-radius:0}
.msg-row.me .msg-bubble{background:#95ec69;color:#333;border-top-right-radius:0}
.msg-time{font-size:11px;color:#bbb;margin-top:3px}

/* 时间分割线 */
.msg-divider{text-align:center;margin:12px 0;font-size:11px;color:#bbb}

/* 输入区 */
.chat-footer{background:#f5f5f5;border-top:1px solid #e0e0e0;padding:12px 20px;flex-shrink:0}
.chat-footer-inner{display:flex;gap:10px;align-items:flex-end}
.chat-footer textarea{flex:1;resize:none;border:1px solid #ddd;border-radius:6px;padding:10px 14px;font-size:14px;font-family:inherit;outline:none;background:#fff;min-height:40px;max-height:120px;line-height:1.5;transition:border-color .2s}
.chat-footer textarea:focus{border-color:#07c160}
.send-btn{padding:10px 24px;background:#07c160;color:#fff;border:none;border-radius:6px;font-size:14px;font-weight:500;cursor:pointer;transition:all .15s;white-space:nowrap;align-self:flex-end}
.send-btn:hover{background:#06ad56}
.send-btn:disabled{background:#a0d9b5;cursor:not-allowed}

/* 连接状态指示器 */
.conn-status{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle}
.conn-status.connected{background:#07c160}
.conn-status.disconnected{background:#f56c6c}
.conn-status.connecting{background:#e6a23c;animation:blink 1s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}

/* 响应式 */
@media(max-width:768px){
  .sidebar{width:0;overflow:hidden;position:absolute;z-index:10;height:100%}
  .sidebar.show{width:260px}
  .app-layout{position:relative}
}
</style>
</head>
<body>

<!-- 登录页 -->
<div class="auth-overlay" id="authPanel">
<div class="auth-card">
  <div class="auth-logo">
    <div class="icon">
      <svg viewBox="0 0 24 24"><path d="M20 2H4c-1.1 0-2 .9-2 2v18l4-4h14c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zm0 14H5.2L4 17.2V4h16v12z"/></svg>
    </div>
    <h1>Coroutine Chat</h1>
    <p>C++20 Coroutine Real-time Chat</p>
  </div>
  <div class="auth-field">
    <label>Username</label>
    <input type="text" id="username" placeholder="Enter username" autocomplete="off">
  </div>
  <div class="auth-field">
    <label>Password</label>
    <input type="password" id="password" placeholder="Enter password" onkeydown="if(event.key==='Enter')doLogin()">
  </div>
  <button class="auth-btn auth-btn-primary" onclick="doLogin()">Sign In</button>
  <button class="auth-btn auth-btn-secondary" onclick="doRegister()">Create Account</button>
  <div class="auth-msg" id="authMsg"></div>
</div>
</div>

<!-- 聊天主界面 -->
<div class="app" id="chatPanel">
<div class="app-layout">
  <!-- 左侧边栏 -->
  <div class="sidebar" id="sidebar">
    <div class="sidebar-header">
      <div class="sidebar-avatar" id="myAvatar"></div>
      <div class="sidebar-info">
        <div class="name" id="myName"></div>
        <div class="status"><span class="conn-status connected" id="connDot"></span><span id="connText">Online</span></div>
      </div>
    </div>
    <div class="sidebar-list">
      <div class="sidebar-item active">
        <div class="room-icon">
          <svg viewBox="0 0 24 24"><path d="M16 11c1.66 0 2.99-1.34 2.99-3S17.66 5 16 5c-1.66 0-3 1.34-3 3s1.34 3 3 3zm-8 0c1.66 0 2.99-1.34 2.99-3S9.66 5 8 5C6.34 5 5 6.34 5 8s1.34 3 3 3zm0 2c-2.33 0-7 1.17-7 3.5V19h14v-2.5c0-2.33-4.67-3.5-7-3.5zm8 0c-.29 0-.62.02-.97.05 1.16.84 1.97 1.97 1.97 3.45V19h6v-2.5c0-2.33-4.67-3.5-7-3.5z"/></svg>
        </div>
        <div class="room-info">
          <div class="room-name">General Chat</div>
          <div class="room-desc" id="roomDesc">Welcome to the chatroom</div>
        </div>
      </div>
    </div>
    <div class="sidebar-bottom">
      <button onclick="doLogout()">Sign Out</button>
    </div>
  </div>

  <!-- 右侧聊天区 -->
  <div class="chat-main">
    <div class="chat-topbar">
      <div style="display:flex;align-items:center">
        <button class="topbar-btn" id="menuBtn" onclick="toggleSidebar()" style="display:none;margin-right:8px">
          <svg viewBox="0 0 24 24"><path d="M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z"/></svg>
        </button>
        <h2>General Chat</h2>
        <span class="online" id="onlineCount"></span>
      </div>
    </div>
    <div id="messages"></div>
    <div class="chat-footer">
      <div class="chat-footer-inner">
        <textarea id="msgInput" rows="1" placeholder="Type a message..." onkeydown="handleKey(event)"></textarea>
        <button class="send-btn" id="sendBtn" onclick="doSend()">Send</button>
      </div>
    </div>
  </div>
</div>
</div>

<script>
let token='', myUser='', ws=null, reconnTimer=null;
const API='';
const AVATAR_COLORS=['#e74c3c','#e67e22','#f1c40f','#2ecc71','#1abc9c','#3498db','#9b59b6','#e84393','#00b894','#6c5ce7','#fd79a8','#0984e3'];

function hashColor(name){
  let h=0;
  for(let i=0;i<name.length;i++) h=name.charCodeAt(i)+((h<<5)-h);
  return AVATAR_COLORS[Math.abs(h)%AVATAR_COLORS.length];
}

function avatarHtml(name,size){
  const c=hashColor(name);
  const ch=name.charAt(0).toUpperCase();
  return '<div style="width:'+size+'px;height:'+size+'px;border-radius:4px;background:'+c+';display:flex;align-items:center;justify-content:center;font-size:'+(size*0.45)+'px;font-weight:600;color:#fff;flex-shrink:0">'+ch+'</div>';
}

function showMsg(id,text,ok){
  const el=document.getElementById(id);
  el.textContent=text;
  el.className='auth-msg '+(ok?'ok':'err');
}

function setConnStatus(status){
  const dot=document.getElementById('connDot');
  const txt=document.getElementById('connText');
  dot.className='conn-status '+status;
  const labels={connected:'Online',disconnected:'Offline',connecting:'Connecting...'};
  txt.textContent=labels[status]||status;
}

let lastMsgDate='';
function addBubble(m){
  const el=document.getElementById('messages');
  const isMe=m.user===myUser;

  const row=document.createElement('div');
  row.className='msg-row'+(isMe?' me':'');

  const av=document.createElement('div');
  av.innerHTML=avatarHtml(m.user,36);

  const body=document.createElement('div');
  body.className='msg-body';

  if(!isMe){
    const nm=document.createElement('div');
    nm.className='msg-name';
    nm.textContent=m.user;
    body.appendChild(nm);
  }

  const bbl=document.createElement('div');
  bbl.className='msg-bubble';
  bbl.textContent=m.text;
  body.appendChild(bbl);

  if(m.time){
    const tm=document.createElement('div');
    tm.className='msg-time';
    tm.textContent=m.time;
    body.appendChild(tm);
  }

  row.appendChild(av.firstChild);
  row.appendChild(body);
  el.appendChild(row);
  el.scrollTop=el.scrollHeight;

  const desc=document.getElementById('roomDesc');
  if(desc) desc.textContent=m.user+': '+m.text;
}

function connectWebSocket(){
  setConnStatus('connecting');
  const proto=location.protocol==='https:'?'wss:':'ws:';
  ws=new WebSocket(proto+'//'+location.host+'/ws?token='+token);
  ws.onopen=function(){
    setConnStatus('connected');
  };
  ws.onmessage=function(e){
    try{const m=JSON.parse(e.data);addBubble(m);}catch(err){}
  };
  ws.onclose=function(){
    setConnStatus('disconnected');
    ws=null;
    if(token){
      reconnTimer=setTimeout(connectWebSocket,2000);
    }
  };
  ws.onerror=function(){};
}

async function doRegister(){
  const u=document.getElementById('username').value.trim();
  const p=document.getElementById('password').value.trim();
  if(!u||!p){showMsg('authMsg','Please enter username and password',false);return}
  try{
    const r=await fetch(API+'/api/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});
    const d=await r.json();
    showMsg('authMsg',d.message||d.error,r.ok);
  }catch(e){showMsg('authMsg','Network error',false)}
}

async function doLogin(){
  const u=document.getElementById('username').value.trim();
  const p=document.getElementById('password').value.trim();
  if(!u||!p){showMsg('authMsg','Please enter username and password',false);return}
  try{
    const r=await fetch(API+'/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});
    const d=await r.json();
    if(r.ok){
      token=d.token; myUser=u;
      document.getElementById('authPanel').style.display='none';
      document.getElementById('chatPanel').style.display='block';
      document.getElementById('myName').textContent=u;
      document.getElementById('myAvatar').innerHTML='';
      const av=document.createElement('div');
      av.innerHTML=avatarHtml(u,40);
      document.getElementById('myAvatar').replaceWith(av.firstChild);
      av.firstChild&&(av.firstChild.id='myAvatar');
      connectWebSocket();
      autoResize();
    }else{showMsg('authMsg',d.error||'Login failed',false)}
  }catch(e){showMsg('authMsg','Network error',false)}
}

function doLogout(){
  token=''; myUser='';
  if(reconnTimer){clearTimeout(reconnTimer);reconnTimer=null;}
  if(ws){ws.close();ws=null;}
  document.getElementById('chatPanel').style.display='none';
  document.getElementById('authPanel').style.display='';
  document.getElementById('messages').innerHTML='';
  lastMsgDate='';
}

function doSend(){
  const input=document.getElementById('msgInput');
  const text=input.value.trim();
  if(!text||!ws||ws.readyState!==1)return;
  input.value='';
  input.style.height='auto';
  ws.send(text);
}

function handleKey(e){
  if(e.key==='Enter'&&!e.shiftKey){
    e.preventDefault();
    doSend();
  }
}

function autoResize(){
  const ta=document.getElementById('msgInput');
  if(!ta)return;
  ta.addEventListener('input',function(){
    this.style.height='auto';
    this.style.height=Math.min(this.scrollHeight,120)+'px';
  });
}

function toggleSidebar(){
  document.getElementById('sidebar').classList.toggle('show');
}

(function(){
  const mq=window.matchMedia('(max-width:768px)');
  function check(){document.getElementById('menuBtn').style.display=mq.matches?'block':'none';}
  mq.addListener(check);check();
})();

function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
</script>
</body>
</html>
)html";
