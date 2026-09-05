// src/apps/simplex/simplex.cpp — the SimpleX front-end app: event parse + WsSimplexApi + module
//
// parse_simplex_events: tolerant translation of daemon frames into SxEvents (canned-JSON tested).
// WsSimplexApi: the real SimplexApi over WsClient — corrId round-trips for commands, queuing any
// events that interleave; next_event pops the queue first. SimplexModule (Task 4): the front-end.
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include "hades/simplex/api.h"
#include "hades/simplex/ws.h"

namespace hades {
namespace {
// {"resp": X}; X may be {"Right": Y}/{"Left": Y} (Haskell Either encoding in some CLI builds).
nlohmann::json unwrap_resp(const nlohmann::json& frame) {
  if (!frame.is_object()) return nullptr;
  auto it = frame.find("resp");
  if (it == frame.end() || !it->is_object()) return nullptr;
  nlohmann::json r = *it;
  if (r.contains("Right") && r["Right"].is_object()) r = r["Right"];
  else if (r.contains("Left") && r["Left"].is_object()) r = r["Left"];
  return r;
}
long long num(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : 0;
}
std::string str(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}
// value(key, default) THROWS type_error.306 when the receiver is not an object, so it is only
// safe after a type check. This is the tolerant accessor: find() is well-defined on any json,
// and a non-object member yields the shared empty object rather than an exception. Returns a
// reference — value() deep-copies.
const nlohmann::json& obj(const nlohmann::json& j, const char* key) {
  static const nlohmann::json kEmpty = nlohmann::json::object();
  auto it = j.find(key);
  return (it != j.end() && it->is_object()) ? *it : kEmpty;
}
}  // namespace

std::vector<SxEvent> parse_simplex_events(const std::string& frame_json) {
  std::vector<SxEvent> out;
  auto frame = nlohmann::json::parse(frame_json, nullptr, false);
  if (frame.is_discarded()) return out;
  const nlohmann::json resp = unwrap_resp(frame);
  if (!resp.is_object()) return out;
  const std::string type = str(resp, "type");

  if (type == "newChatItems") {
    auto items = resp.find("chatItems");
    if (items == resp.end() || !items->is_array()) return out;
    for (const auto& it : *items) {
      if (!it.is_object()) continue;
      const auto& ci = it.value("chatInfo", nlohmann::json::object());
      const auto& item = it.value("chatItem", nlohmann::json::object());
      if (str(ci, "type") != "direct") continue;                        // v1: DMs only
      const auto& contact = ci.value("contact", nlohmann::json::object());
      const auto& dir = obj(item, "chatDir");
      if (str(dir, "type") != "directRcv") continue;                    // skip our own echoes
      const auto& content = item.value("content", nlohmann::json::object());
      if (str(content, "type") != "rcvMsgContent") continue;
      const auto& mc = content.value("msgContent", nlohmann::json::object());
      const std::string mct = str(mc, "type");
      if (mct == "text") {
        SxEvent ev;
        ev.kind = SxEvent::Kind::Text;
        ev.contact_id = num(contact, "contactId");
        ev.display_name = str(contact, "localDisplayName");
        ev.text = str(mc, "text");
        if (ev.contact_id != 0 && !ev.text.empty()) out.push_back(std::move(ev));
      } else if (mct == "voice") {
        // The audio is NOT in this frame: it arrives via the file transfer, and the item is only
        // readable once rcvFileComplete fires. A voice item with no `file` is malformed -> drop.
        const auto f = item.find("file");
        if (f == item.end() || !f->is_object()) continue;
        SxEvent ev;
        ev.kind = SxEvent::Kind::Voice;
        ev.contact_id = num(contact, "contactId");
        ev.display_name = str(contact, "localDisplayName");
        ev.file_id = num(*f, "fileId");
        ev.file_size = num(*f, "fileSize");
        ev.file_name = str(*f, "fileName");   // extension only, validated at use — see api.h
        const auto d = mc.find("duration");
        ev.duration = (d != mc.end() && d->is_number_integer()) ? d->get<int>() : 0;
        if (ev.contact_id != 0 && ev.file_id != 0) out.push_back(std::move(ev));
      }                                                                 // other types: ignored
    }
  } else if (type == "receivedContactRequest") {
    const auto& cr = resp.value("contactRequest", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::ContactRequest;
    ev.request_id = num(cr, "contactRequestId");
    ev.display_name = str(cr, "localDisplayName");
    if (ev.request_id != 0) out.push_back(std::move(ev));
  } else if (type == "contactConnected") {
    const auto& c = resp.value("contact", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::Connected;
    ev.contact_id = num(c, "contactId");
    ev.display_name = str(c, "localDisplayName");
    if (ev.contact_id != 0) out.push_back(std::move(ev));
  } else if (type == "rcvFileComplete") {
    // chatItem is mandatory on this frame; the file id lives on the item's file object.
    const auto& aci = resp.value("chatItem", nlohmann::json::object());
    const auto& item = obj(aci, "chatItem");
    const auto& file = obj(item, "file");
    SxEvent ev;
    ev.kind = SxEvent::Kind::FileDone;
    ev.file_id = num(file, "fileId");
    if (ev.file_id != 0) out.push_back(std::move(ev));
  } else if (type == "rcvFileError" || type == "rcvFileSndCancelled" ||
             type == "rcvFileAcceptedSndCancelled") {
    // chatItem_ is OPTIONAL on rcvFileError, so rcvFileTransfer is the only reliable id source.
    // rcvFileAcceptedSndCancelled has the same shape (the sender cancelled after we accepted)
    // and is terminal too: without it that transfer would sit in the pending table until it was
    // evicted, and the sender would get no reply at all.
    // rcvFileWarning has the same shape but is NOT terminal (it fires when CLI settings block a
    // file server), so it is deliberately not handled here.
    const auto& rft = resp.value("rcvFileTransfer", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::FileFailed;
    ev.file_id = num(rft, "fileId");
    ev.display_name = str(rft, "senderDisplayName");
    if (ev.file_id != 0) out.push_back(std::move(ev));
  }
  return out;
}

// ── WsSimplexApi: the real seam impl over WsClient (file-local; exposed via the factory) ─────
namespace {
constexpr double kCmdTimeoutS = 15.0;   // one command round-trip against the LOCAL daemon

class WsSimplexApi : public SimplexApi {
 public:
  WsSimplexApi(std::string host, int port, double connect_timeout_s)
      : host_(std::move(host)), port_(port), connect_timeout_s_(connect_timeout_s) {}

  bool reconnect() override {
    pending_.clear();
    return ws_.connect(host_, port_, connect_timeout_s_);
  }

  SxStatus next_event(double timeout_s, SxEvent& out) override {
    if (!pending_.empty()) {
      out = pending_.front();
      pending_.pop_front();
      return SxStatus::Event;
    }
    std::string frame;
    switch (ws_.recv_text(timeout_s, frame)) {
      case WsRecv::Timeout: return SxStatus::Timeout;
      case WsRecv::Closed: return SxStatus::Closed;
      case WsRecv::Error: return SxStatus::Error;
      case WsRecv::Text: break;
    }
    for (auto& ev : parse_simplex_events(frame)) pending_.push_back(std::move(ev));
    if (pending_.empty()) return SxStatus::Timeout;   // frame parsed to nothing: caller re-loops
    out = pending_.front();
    pending_.pop_front();
    return SxStatus::Event;
  }

  bool send_text(long long contact_id, const std::string& text) override {
    nlohmann::json msgs = nlohmann::json::array(
        {{{"msgContent", {{"type", "text"}, {"text", text}}}}});
    return command_ok_("/_send @" + std::to_string(contact_id) + " json " + msgs.dump(),
                       "newChatItems");
  }

  bool accept_request(long long request_id) override {
    return command_ok_("/_accept " + std::to_string(request_id), "acceptingContactRequest");
  }

  bool receive_file(long long file_id, const std::string& dest_path) override {
    // /freceive <fileId>[ encrypt=on|off][ <filePath>] — encrypt=off so the bytes on disk are
    // readable (an encrypted CryptoFile carries cryptoArgs and cannot be POSTed to an STT backend).
    // The daemon's command grammar is space-delimited with no quoting, so a path containing a
    // space would silently mis-parse (the caller's temp dir honours TMPDIR): refuse it here.
    if (dest_path.find(' ') != std::string::npos) return false;
    return command_ok_("/freceive " + std::to_string(file_id) + " encrypt=off " + dest_path,
                       "rcvFileAccepted");
  }

 private:
  // Send {corrId,cmd}; read until the matching resp (events seen meanwhile are queued).
  // ok iff the resp type equals ok_type. Total wait bounded by kCmdTimeoutS even across a
  // stream of non-matching frames. Timeout/close/error -> false (fail-soft; caller logs).
  bool command_ok_(const std::string& cmd, const char* ok_type) {
    if (!ws_.connected()) return false;
    const std::string corr = std::to_string(++corr_);
    nlohmann::json env{{"corrId", corr}, {"cmd", cmd}};
    if (!ws_.send_text(env.dump())) return false;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      const double remaining = kCmdTimeoutS - elapsed;
      if (remaining <= 0.0) return false;
      std::string frame;
      if (ws_.recv_text(remaining, frame) != WsRecv::Text) return false;
      auto j = nlohmann::json::parse(frame, nullptr, false);
      if (j.is_object() && j.value("corrId", "") == corr) {
        const nlohmann::json resp = unwrap_resp(j);
        return resp.is_object() && str(resp, "type") == ok_type;
      }
      for (auto& ev : parse_simplex_events(frame)) pending_.push_back(std::move(ev));
    }
  }

  std::string host_;
  int port_;
  double connect_timeout_s_;
  WsClient ws_;
  long long corr_ = 0;
  std::deque<SxEvent> pending_;
};
}  // anonymous namespace

std::unique_ptr<SimplexApi> make_ws_simplex_api(std::string host, int port,
                                                double connect_timeout_s) {
  return std::make_unique<WsSimplexApi>(std::move(host), port, connect_timeout_s);
}
}  // namespace hades

// ── SimplexModule: event loop, allowlist, turn driving, text y/N confirms, voice input ───────
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <sstream>
#include "hades/module/simplex_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include "hades/launcher.h"          // MalConfig
#include "hades/stt/provider.h"       // SttProvider / SttResult
#include "hades/telegram/parse.h"    // split_message
#include "hades/timeouts.h"          // kDefaultTurnIdleTimeoutS

namespace hades {
namespace {
constexpr std::size_t kSimplexSplit = 4000;

std::string trim(const std::string& s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  auto b = std::find_if(s.begin(), s.end(), ns);
  auto e = std::find_if(s.rbegin(), s.rend(), ns).base();
  return (b < e) ? std::string(b, e) : std::string{};
}
bool is_yes(const std::string& raw) {
  std::string t = trim(raw);
  std::transform(t.begin(), t.end(), t.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return t == "y" || t == "yes";
}
// Numeric token (all digits) -> id; anything else -> display name. Comma-separated, trimmed.
void split_contacts(const std::string& raw, std::set<long long>& ids, std::set<std::string>& names) {
  std::stringstream ss(raw);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    tok = trim(tok);
    if (tok.empty()) continue;
    if (std::all_of(tok.begin(), tok.end(), [](unsigned char c) { return std::isdigit(c); }))
      ids.insert(std::stoll(tok));
    else
      names.insert(tok);
  }
}
}  // namespace

SimplexModule::SimplexModule(std::unique_ptr<SimplexApi> api) : api_(std::move(api)) {}

SimplexModule::~SimplexModule() {
  stop_.store(true);
  stop_cv_.notify_all();
  // NOTE: a live next_event can hold the join up to ~poll_timeout_s_ (the WS read deadline).
  if (ev_thread_.joinable()) ev_thread_.join();
  stop_daemon_();   // AFTER the join — the event thread may be mid-socket-op against the daemon
  // Reclaim any audio the per-entry remove() calls could not: in-flight transfers at shutdown,
  // a /freceive the daemon completed after receive_file reported failure, evicted partials.
  // Runs LAST: the event thread is joined (it alone touches pending_voice_) and the daemon is
  // dead (it alone still writes here). Guarded to our own per-pid directory — never a bare
  // temp_directory_path(), which an empty voice_tmp_dir_ would otherwise mean.
  if (!voice_tmp_dir_.empty() &&
      voice_tmp_dir_.filename().string().rfind("hades-sx-voice-", 0) == 0) {
    std::error_code ec;
    std::filesystem::remove_all(voice_tmp_dir_, ec);
  }
}

void SimplexModule::spawn_daemon_() {
  if (daemon_argv_.empty() || daemon_pid_ > 0) return;
  std::vector<char*> argv;
  argv.reserve(daemon_argv_.size() + 1);
  for (auto& a : daemon_argv_) argv.push_back(a.data());
  argv.push_back(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) {
    std::cerr << "hades: simplex: cannot fork daemon (" << daemon_argv_[0] << ")\n";
    return;   // fail-soft: the reconnect loop will keep reporting the daemon unreachable
  }
  if (pid == 0) {
    // Child: stdin -> /dev/null, stdout+stderr -> append log (an auto-started daemon must not
    // fail invisibly, and must not scribble on the libedit REPL). exec only returns on failure.
    // PDEATHSIG: a hades-SPAWNED daemon must die with hades — without this, a SIGINT/SIGTERM
    // shutdown (the NORMAL exit on a headless/systemd roster, where no dtor runs) would orphan
    // the daemon still holding the port, and the next launch would spawn a second one into a
    // bind failure.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    ::mkdir(".hades", 0755);
    if (const int fd = ::open(".hades/simplex-chat.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
        fd >= 0) {
      ::dup2(fd, 1);
      ::dup2(fd, 2);
      if (fd > 2) ::close(fd);
    }
    if (const int nul = ::open("/dev/null", O_RDONLY); nul >= 0) {
      ::dup2(nul, 0);
      if (nul > 2) ::close(nul);
    }
    ::execvp(argv[0], argv.data());
    // execvp itself prints nothing — write the failure to fd 2 (= the log) so a mistyped
    // `command` is distinguishable from a daemon that started and then crashed.
    constexpr char m[] = "hades: simplex: exec failed for daemon command\n";
    [[maybe_unused]] auto r = ::write(2, m, sizeof(m) - 1);
    ::_exit(127);
  }
  daemon_pid_ = static_cast<int>(pid);
}

void SimplexModule::stop_daemon_() {
  if (daemon_pid_ <= 0) return;
  ::kill(daemon_pid_, SIGTERM);
  for (int i = 0; i < 20; ++i) {   // ~2s grace, then force — never hang shutdown on the daemon
    if (::waitpid(daemon_pid_, nullptr, WNOHANG) != 0) { daemon_pid_ = 0; return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ::kill(daemon_pid_, SIGKILL);
  ::waitpid(daemon_pid_, nullptr, 0);
  daemon_pid_ = 0;
}

double SimplexModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void SimplexModule::on_start(const Block& cfg, Blackboard&) {
  // allow_contacts is REQUIRED — an open bot means anyone who connects can drive the agent's
  // tools (telegram allow_users precedent). Fail fast and loud.
  if (!cfg.kv.count("allow_contacts"))
    throw MalConfig("simplex module requires allow_contacts (contact ids and/or display names)");
  split_contacts(cfg.kv.at("allow_contacts"), allow_ids_, allow_names_);
  if (allow_ids_.empty() && allow_names_.empty())
    throw MalConfig("simplex module requires a non-empty allow_contacts");
  if (cfg.kv.count("auto_accept")) set_bool_on_string(cfg.kv.at("auto_accept"), auto_accept_);
  if (cfg.kv.count("notify_contact")) notify_contact_ = trim(cfg.kv.at("notify_contact"));
  if (cfg.kv.count("connect_timeout_s")) {
    double t = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("connect_timeout_s"), t)) connect_timeout_s_ = t;
  }
  if (cfg.kv.count("command")) {                  // auto-start the daemon (start() spawns it)
    std::istringstream is(cfg.kv.at("command"));
    std::string w;
    while (is >> w) daemon_argv_.push_back(w);
  }
  if (api_) return;                               // injected (tests)
  const std::string host = cfg.kv.count("host") ? cfg.kv.at("host") : "127.0.0.1";
  int port = 5225;
  if (cfg.kv.count("port")) {
    try { port = std::stoi(cfg.kv.at("port")); } catch (...) { port = 5225; }
  }
  api_ = make_ws_simplex_api(host, port, connect_timeout_s_);
}

void SimplexModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    pending_confirm_ = e.value;
  });
  // Notify sink (heartbeat etc.): the subscriber runs on WHATEVER thread pumps the post — for
  // a heartbeat tick's notify that is the HEARTBEAT timer thread — so it must NOT touch api_
  // or known_ids_: the event thread owns the one persistent socket (WsClient is single-threaded
  // by contract; the telegram sink can send inline only because each send is a fresh stateless
  // HTTP call). Queue the text under a mutex; the event loop drains it in step_once. Delivery
  // latency <= poll_timeout_s_ — fine for notifications.
  bb.subscribe("NOTIFY_USER", [this](const Entry& e) {
    if (notify_contact_.empty()) return;
    std::string text;
    if (e.value.is_object()) text = e.value.value("text", "");
    else if (e.value.is_string()) text = e.value.get<std::string>();
    if (text.empty()) return;
    std::lock_guard<std::mutex> lk(notify_mu_);
    notify_queue_.push_back(std::move(text));
  });
}

// Drain queued NOTIFY_USER texts — called ONLY from the event thread (step_once), the one
// thread allowed to touch api_ and known_ids_. A name-form notify_contact resolves via
// known_ids_; unresolved -> logged skip (texts dropped — best-effort, telegram parity).
void SimplexModule::drain_notifies_() {
  std::vector<std::string> pending;
  {
    std::lock_guard<std::mutex> lk(notify_mu_);
    pending.swap(notify_queue_);
  }
  if (pending.empty() || !api_) return;
  long long cid = 0;
  if (std::all_of(notify_contact_.begin(), notify_contact_.end(),
                  [](unsigned char c) { return std::isdigit(c); })) {
    cid = std::stoll(notify_contact_);
  } else {
    auto it = known_ids_.find(notify_contact_);
    if (it != known_ids_.end()) cid = it->second;
  }
  if (cid == 0) {
    std::cerr << "hades: simplex notify skipped (contact not yet known: " << notify_contact_ << ")\n";
    return;
  }
  for (const auto& text : pending) {
    try {
      if (!api_->send_text(cid, text))
        std::cerr << "hades: simplex notify send failed (contact " << cid << ")\n";
    } catch (...) { /* fail-soft */ }
  }
}

bool SimplexModule::allowed_(const SxEvent& ev) const {
  return allow_ids_.count(ev.contact_id) || allow_names_.count(ev.display_name);
}

void SimplexModule::send_reply_(long long contact_id, const std::string& text) {
  for (const auto& chunk : split_message(text, kSimplexSplit))
    if (!api_->send_text(contact_id, chunk))
      std::cerr << "hades: simplex send failed (reply dropped; history is persisted)\n";
}

void SimplexModule::drive_turn_(long long contact_id, const nlohmann::json& post_value,
                                const char* key) {
  std::lock_guard<std::mutex> lk(turn_mu_());
  my_turn_ = true;
  struct Reset { bool& mine; ~Reset() { mine = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  pending_confirm_ = nullptr;
  bb_->post("TURN_ORIGIN", "human", "simplex");
  bb_->post(key, post_value, "simplex");
  const bool done = bb_->run_until(
      [this] { return got_reply_ || !pending_confirm_.is_null(); }, effective_timeout_());
  if (!done) {
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "simplex");
    bb_->pump();
    send_reply_(contact_id, "[timed out]");
    return;
  }
  if (got_reply_) {
    send_reply_(contact_id, last_reply_);
    return;
  }
  // Confirm-gated: send a y/N text prompt; the next message from this contact answers it.
  const std::string id = pending_confirm_.value("id", "");
  const std::string prompt = pending_confirm_.value("prompt", "");
  outstanding_confirm_id_ = id;
  outstanding_contact_id_ = contact_id;
  send_reply_(contact_id, (prompt.empty() ? std::string("confirm?") : prompt) +
                              "\n(reply y to approve, anything else to deny)");
}

void SimplexModule::handle_text_(const SxEvent& ev) {
  // An outstanding confirm is answered by the NEXT message from the SAME contact.
  if (!outstanding_confirm_id_.empty() && ev.contact_id == outstanding_contact_id_) {
    const std::string id = outstanding_confirm_id_;
    outstanding_confirm_id_.clear();
    drive_turn_(ev.contact_id, nlohmann::json{{"id", id}, {"approved", is_yes(ev.text)}},
                "CONFIRM_RESPONSE");
    return;
  }
  drive_turn_(ev.contact_id, nlohmann::json(ev.text), "USER_MESSAGE");
}

// A voice offer: gate it, then ask the daemon to drop the bytes (UNENCRYPTED) at a temp path we
// name. The audio is NOT here yet — the turn happens later, on the matching FileDone.
void SimplexModule::handle_voice_(const SxEvent& ev) {
  // An outstanding y/N confirm blocks voice from this contact. Refusing is the deliberate
  // choice: letting the transcript ANSWER the confirm would turn a mishearing into
  // approved:true on a confirm-gated (potentially destructive) action, and approval is a
  // security boundary that must not sit behind a fuzzy channel; silently denying would throw
  // away what the user actually said. So we say what is blocking, leave the confirm armed for
  // the next TEXT, and accept nothing off the daemon.
  if (!outstanding_confirm_id_.empty() && ev.contact_id == outstanding_contact_id_) {
    send_reply_(ev.contact_id,
                "I'm still waiting on the y/n above — please answer that first, then send the "
                "voice message again.");
    return;
  }
  if (!stt_) { send_reply_(ev.contact_id, "Voice messages aren't enabled on this agent."); return; }
  // A non-positive size means we do NOT know how big the file is: the field was absent or
  // mistyped (the parse fails closed to 0), or an out-of-int64 unsigned wrapped negative. The
  // size cap is a load-bearing safety control here — it is the reason we accept files explicitly
  // instead of letting the daemon auto-accept — so an unknown size is refused, not waved through
  // by `0 > cap` being false. Gated here rather than dropped in the parser so the sender is told.
  // Both cases refuse; the reason given is the true one, since "too large" for a size we never
  // read would send the sender off shortening a message that was never the problem.
  if (ev.file_size <= 0) {
    send_reply_(ev.contact_id,
                "I couldn't tell how big that voice message is, so I didn't download it.");
    return;
  }
  if (ev.file_size > voice_max_bytes_) {
    send_reply_(ev.contact_id, "That voice message is too large for me to process.");
    return;
  }
  // No directory to put the bytes in: start() could not make one (either failure mode — see the
  // flag's comment), or ours went away since. Refuse rather than hand the daemon a path that
  // cannot be written, or let voice_temp_path_ throw into step_once's catch (logged, no reply).
  if (!voice_dir_ok_) {
    send_reply_(ev.contact_id, "I can't store voice messages right now.");
    return;
  }
  if (!voice_tmp_dir_.empty()) {
    std::error_code ec;
    if (!std::filesystem::is_directory(voice_tmp_dir_, ec)) {
      send_reply_(ev.contact_id, "I can't store voice messages right now.");
      return;
    }
  }
  // Bound the table BEFORE inserting: drop the oldest pending transfer (and its temp file) so a
  // stream of never-completing offers cannot grow it.
  while (pending_voice_.size() >= kMaxPendingVoice) {
    auto oldest = pending_voice_.begin();
    std::error_code ec;
    std::filesystem::remove(oldest->second.path, ec);
    pending_voice_.erase(oldest);
  }
  const std::string dest = voice_temp_path_(ev.file_id, ev.file_name);
  if (!api_->receive_file(ev.file_id, dest)) {
    // No pending entry, so nothing will reclaim `dest` by path — but the daemon may have taken
    // the /freceive before the reply timed out and may still write the file. That orphan is
    // what the per-pid directory exists for; the dtor takes it.
    send_reply_(ev.contact_id, "I couldn't download that voice message.");
    return;
  }
  pending_voice_[ev.file_id] = PendingVoice{ev.contact_id, dest};
}

// The bytes landed: transcribe and drive a NORMAL turn with the transcript, indistinguishable
// downstream from a typed message. Fail-soft — every failure is a short reply, no exception
// escapes the event loop, and the temp file goes on every exit path.
void SimplexModule::handle_file_done_(const SxEvent& ev) {
  auto it = pending_voice_.find(ev.file_id);
  if (it == pending_voice_.end()) return;        // not ours (or already reclaimed) — ignore
  const PendingVoice pv = it->second;
  pending_voice_.erase(it);
  // The SAME security boundary handle_voice_ guards, entered through the other door: a confirm
  // can be armed AFTER the offer was accepted but BEFORE the bytes land, which is the normal
  // case for a transfer taking seconds — and accepting an offer sends no reply, so the contact
  // has no reason to wait. Without this guard: voice offer accepted -> contact types something
  // that arms confirm A -> the bytes arrive, this drives a fresh USER_MESSAGE (the Arbiter's
  // USER_MESSAGE handler does NOT clear_pending(), so A stays armed on both sides) -> the voice
  // turn asks its own question -> the contact answers "y" -> handle_text_ routes that y to the
  // OUTSTANDING confirm and action A is dispatched. They approved one thing and got another.
  // So: reclaim the audio, tell them what is blocking, and drive no turn.
  if (!outstanding_confirm_id_.empty() && pv.contact_id == outstanding_contact_id_) {
    std::error_code ec;
    std::filesystem::remove(pv.path, ec);
    send_reply_(pv.contact_id,
                "I'm still waiting on the y/n above — please answer that first, then send the "
                "voice message again.");
    return;
  }
  std::string transcript;
  if (!stt_) {
    // Unreachable today — an entry only exists because handle_voice_ saw a provider. Kept as
    // defence, but with its own line: a default SttResult here would have logged "transcribe
    // failed: " with nothing after it, which reads like a provider that lost its error text.
    std::cerr << "hades: simplex: voice file " << ev.file_id
              << " completed with no STT provider; dropped\n";
  } else {
    try {
      SttResult r = stt_->transcribe(pv.path);
      if (r.ok) transcript = trim(r.text);
      else std::cerr << "hades: simplex transcribe failed: " << r.error << "\n";
    } catch (...) { transcript.clear(); }        // providers promise not to throw; belt+braces
  }
  std::error_code ec;
  std::filesystem::remove(pv.path, ec);          // always, success or failure
  if (transcript.empty()) {
    send_reply_(pv.contact_id, "Sorry, I didn't catch that.");
    return;
  }
  drive_turn_(pv.contact_id, nlohmann::json(transcript), "USER_MESSAGE");
}

void SimplexModule::handle_file_failed_(const SxEvent& ev) {
  auto it = pending_voice_.find(ev.file_id);
  if (it == pending_voice_.end()) return;
  const PendingVoice pv = it->second;
  pending_voice_.erase(it);
  std::error_code ec;
  std::filesystem::remove(pv.path, ec);
  send_reply_(pv.contact_id, "That voice message didn't come through.");
}

void SimplexModule::handle_event_(const SxEvent& ev) {
  // Learn name->id from any event carrying both (notify_contact name resolution).
  if (ev.contact_id != 0 && !ev.display_name.empty()) known_ids_[ev.display_name] = ev.contact_id;
  switch (ev.kind) {
    case SxEvent::Kind::Text:
      if (allowed_(ev)) handle_text_(ev);       // non-allowed: silently dropped
      break;
    case SxEvent::Kind::Voice:
      if (allowed_(ev)) handle_voice_(ev);      // non-allowed: silently dropped, as for Text
      break;
    // FileDone/FileFailed carry no contact, so they cannot be allowlist-gated; the pending-table
    // lookup IS the gate — a file id we never accepted is ignored.
    case SxEvent::Kind::FileDone:   handle_file_done_(ev); break;
    case SxEvent::Kind::FileFailed: handle_file_failed_(ev); break;
    case SxEvent::Kind::ContactRequest:
      if (auto_accept_) {
        if (!api_->accept_request(ev.request_id))
          std::cerr << "hades: simplex accept_request failed (" << ev.request_id << ")\n";
      } else {
        std::cerr << "hades: simplex contact request from \"" << ev.display_name
                  << "\" ignored (auto_accept=false; accept it in the simplex-chat CLI)\n";
      }
      break;
    case SxEvent::Kind::Connected:
      std::cerr << "hades: simplex contact connected: " << ev.display_name << "\n";
      break;
    case SxEvent::Kind::None:
      break;
  }
}

bool SimplexModule::step_once() {
  try {
    drain_notifies_();   // event thread only — the one place notify sends touch the socket
    SxEvent ev;
    switch (api_->next_event(poll_timeout_s_, ev)) {
      case SxStatus::Event:
        handle_event_(ev);
        return true;
      case SxStatus::Timeout:
        return true;
      case SxStatus::Closed:
      case SxStatus::Error:
        return false;
    }
  } catch (const std::exception& e) {
    std::cerr << "hades: simplex event error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "hades: simplex event error (unknown)\n";
  }
  return true;
}

void SimplexModule::run_loop_() {
  // Initial connect + reconnect-on-drop, with an interruptible backoff.
  bool need_connect = true;
  while (!stop_.load()) {
    if (need_connect) {
      if (!api_->reconnect()) {
        // Backoff base = connect_timeout_s (spec); interruptible so the dtor never waits it out.
        std::cerr << "hades: simplex daemon unreachable; retrying\n";
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait_for(lk, std::chrono::duration<double>(connect_timeout_s_),
                          [this] { return stop_.load(); });
        continue;
      }
      need_connect = false;
    }
    if (!step_once()) need_connect = true;
  }
}

void SimplexModule::start() {
  if (ev_thread_.joinable()) return;   // idempotent
  // Own temp directory for voice audio, set BEFORE the thread exists (the event thread only
  // ever reads it). Fail-soft: on EITHER failure voice_dir_ok_ stays false and handle_voice_
  // refuses voice with a reply — an unwritable temp dir must not take the front-end down, and
  // must not leave the sender with silence either. Both failures need the flag: a
  // create_directories error leaves voice_tmp_dir_ SET (so an is_directory check would catch
  // it), but a throwing temp_directory_path() leaves it EMPTY, which is indistinguishable from
  // "start() was never called" and would sail past a path-based check into a second throw.
  voice_dir_ok_ = false;
  try {
    voice_tmp_dir_ =
        std::filesystem::temp_directory_path() / ("hades-sx-voice-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(voice_tmp_dir_, ec);
    // Kept set even on failure so the dtor still reclaims anything the daemon wrote there.
    if (ec) std::cerr << "hades: simplex: cannot create voice temp dir " << voice_tmp_dir_ << " ("
                      << ec.message() << "); voice input will be refused\n";
    else voice_dir_ok_ = true;
  } catch (const std::exception& e) {
    std::cerr << "hades: simplex: no usable temp dir for voice (" << e.what()
              << "); voice input will be refused\n";
  }
  spawn_daemon_();                     // before the thread; reconnect backoff absorbs its boot
  ev_thread_ = std::thread([this] { run_loop_(); });
}

namespace {
// The extension to give the temp file, taken from the SENDER's offered name.
//
// It matters because of the DEFAULT STT transport: HttpSttProvider hands the path to
// cpr::File, libcurl sets the multipart part's remote filename to the local BASENAME, and an
// OpenAI-compatible /audio/transcriptions validates the audio format by that name's extension
// (flac,m4a,mp3,mp4,mpeg,mpga,oga,ogg,wav,webm). A ".bin" is rejected with a 400 — every voice
// message would fail. TelegramModule names its temp file ".oga" for exactly this reason.
//
// The offered name is attacker-controlled and must NEVER reach the path: only a validated
// extension is taken, and the stem stays our own file-id string. A name carrying a path
// separator, "..", or a NUL is not used at all — those cannot appear in a real voice offer, so
// falling back costs nothing and keeps the traversal question from arising twice.
std::string voice_ext_from_name(const std::string& name) {
  const std::string fallback = "m4a";                     // SimpleX's own voice format
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      name.find("..") != std::string::npos || name.find('\0') != std::string::npos)
    return fallback;
  const auto dot = name.rfind('.');
  if (dot == std::string::npos) return fallback;
  std::string ext = name.substr(dot + 1);
  if (ext.empty() || ext.size() > 5) return fallback;
  for (char& c : ext) {
    if (!std::isalnum(static_cast<unsigned char>(c))) return fallback;
    // Lowercased because the backend's format list is lowercase and we do not know whether it
    // matches case-insensitively — ".M4A" from some client must not reintroduce the 400.
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext;
}
}  // namespace

// Where the daemon should drop file_id's bytes. Inside our per-pid directory once start() has
// made one; otherwise straight in the system temp dir (start() not called — tests). The file
// name carries the id in BOTH forms so a stray temp is traceable to its transfer; the suffix
// comes from the offer because the http STT backend format-checks it (see above).
std::string SimplexModule::voice_temp_path_(long long file_id,
                                            const std::string& offered_name) const {
  const std::filesystem::path dir =
      voice_tmp_dir_.empty() ? std::filesystem::temp_directory_path() : voice_tmp_dir_;
  return (dir / ("hades-sx-voice-" + std::to_string(file_id) + "." +
                 voice_ext_from_name(offered_name)))
      .string();
}

void SimplexModule::wait() {
  if (ev_thread_.joinable()) ev_thread_.join();
}
}  // namespace hades
