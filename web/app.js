// hades web UI — talks to the existing POST /chat + POST /confirm JSON API.
const log = document.getElementById('log');
const form = document.getElementById('composer');
const input = document.getElementById('input');

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
  d.innerHTML = '<span class="label">' + role + '&gt; </span>' + escapeText(text);
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
  const resolve = (approved) => { actions.remove(); sendConfirm(id, approved); };
  approve.addEventListener('click', () => resolve(true));
  deny.addEventListener('click', () => resolve(false));
}

async function postJson(url, body) {
  const r = await fetch(url, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
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
  send(t);
});
