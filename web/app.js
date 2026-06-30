// hades web UI — talks to the existing POST /chat + POST /confirm JSON API.
const log = document.getElementById('log');
const form = document.getElementById('composer');
const input = document.getElementById('input');
const TOOL_RESULT_MAX = 500;  // dim tool-result entries are truncated for display

document.getElementById('clear').addEventListener('click', () => {
  log.innerHTML = '';
  input.focus();
});

function escapeText(s) {
  return s.replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}
function scrollDown() { log.scrollTop = log.scrollHeight; }

function addMessage(role, text) {
  const d = document.createElement('div');
  d.className = 'msg ' + role;
  d.innerHTML = '<span class="label">' + escapeText(role) + '&gt; </span>' + escapeText(text);
  log.appendChild(d);
  scrollDown();
}
function addError(text) {
  const d = document.createElement('div');
  d.className = 'msg error';
  d.textContent = text;
  log.appendChild(d);
  scrollDown();
}
function addConfirm(id, prompt) {
  const box = document.createElement('div');
  box.className = 'confirm';
  box.innerHTML = '<span class="label">confirm: </span>' + escapeText(prompt);
  const actions = document.createElement('div');
  actions.className = 'actions';
  const approve = document.createElement('button');
  approve.className = 'approve'; approve.type = 'button'; approve.textContent = 'Approve';
  const deny = document.createElement('button');
  deny.className = 'deny'; deny.type = 'button'; deny.textContent = 'Deny';
  actions.appendChild(approve);
  actions.appendChild(deny);
  box.appendChild(actions);
  log.appendChild(box);
  scrollDown();
  const resolve = (approved) => {
    actions.remove();
    const note = document.createElement('span');
    note.className = 'result';
    note.textContent = approved ? ' → approved' : ' → denied';
    box.appendChild(note);
    sendConfirm(id, approved);
  };
  approve.addEventListener('click', () => resolve(true));
  deny.addEventListener('click', () => resolve(false));
}

function addToolCall(name, args) {
  const d = document.createElement('div');
  d.className = 'msg tool-call';
  const n = (typeof name === 'string') ? name : String(name);
  const a = (typeof args === 'string') ? args : JSON.stringify(args);
  d.innerHTML = '<span class="label">\u{1F527} ' + escapeText(n) + ' </span>' + escapeText(a);
  log.appendChild(d);
  scrollDown();
}
function addToolResult(content) {
  const d = document.createElement('div');
  d.className = 'msg tool-result';
  let s = (typeof content === 'string') ? content : JSON.stringify(content);
  if (s.length > TOOL_RESULT_MAX) s = s.slice(0, TOOL_RESULT_MAX) + '…';
  d.innerHTML = '<span class="label">→ </span>' + escapeText(s);
  log.appendChild(d);
  scrollDown();
}
function renderHistory(msgs) {
  for (const m of msgs) {
    if (!m || typeof m !== 'object') continue;
    if (m.role === 'user' && typeof m.content === 'string') {
      addMessage('user', m.content);
    } else if (m.role === 'assistant' && typeof m.content === 'string' && m.content.length) {
      addMessage('assistant', m.content);
    } else if (m.role === 'assistant' && Array.isArray(m.tool_calls)) {
      for (const tc of m.tool_calls) {
        const fn = (tc && tc.function) ? tc.function : {};
        addToolCall(fn.name || '?', fn.arguments != null ? fn.arguments : '');
      }
    } else if (m.role === 'tool') {
      addToolResult(m.content != null ? m.content : '');
    }
    // any other shape: skip (tolerant; never throw)
  }
}
async function loadHistory() {
  try {
    const r = await fetch('/history', {headers: {'X-Hades': '1'}});
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const data = await r.json();
    if (data && Array.isArray(data.history)) renderHistory(data.history);
  } catch (e) {
    addError('history load failed: ' + e.message);
  }
}
async function postJson(url, body) {
  const r = await fetch(url, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Hades': '1'},
    body: JSON.stringify(body),
  });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}
function render(res) {
  if (res && res.needs_confirm) addConfirm(res.id, res.prompt);
  else if (res && typeof res.reply === 'string' && res.reply.length) addMessage('assistant', res.reply);
}
async function send(text) {
  addMessage('user', text);
  try { render(await postJson('/chat', {message: text})); }
  catch (e) { addError('error: ' + e.message); }
}
async function sendConfirm(id, approved) {
  try { render(await postJson('/confirm', {id: id, approved: approved})); }
  catch (e) { addError('error: ' + e.message); }
}

form.addEventListener('submit', (e) => {
  e.preventDefault();
  const t = input.value.trim();
  if (!t) return;
  input.value = '';
  const btn = form.querySelector('button[type=submit]');
  btn.disabled = true;
  send(t).finally(() => { btn.disabled = false; });
});

loadHistory();
