// src/apps/arbiter/arbiter.cpp — decision core: USER_MESSAGE -> LLM -> act -> gate
//
// Subscribes to USER_MESSAGE, LLM_RESPONSE, TOOL_RESULT, and CONFIRM_RESPONSE
// on the Blackboard. Per turn: posts LLM_REQUEST with conversation history and
// tool specs; on LLM_RESPONSE builds an Action, runs it through registered
// Objectives (veto / confirm gate), then drives TOOL_REQUEST or
// ASSISTANT_MESSAGE; loops tool results back via start_turn() up to kMaxSteps.

#include "hades/arbiter.h"
#include "hades/blackboard.h"
#include "hades/compact/compact.h"   // digest_span, sidecar codec, session_stem/path (compaction)
#include "hades/prompt.h"   // read_memory_layer
#include "hades/session_id.h"   // logical_date, unique_fresh_path, lock_session_file (rotation)
#include "hades/session_history.h"   // read_session_jsonl (shared tolerant parse)
#include <algorithm>   // std::find (merge_memory_blocks dedup)
#include <cstdio>   // std::remove (sidecar tmp cleanup)
#include <ctime>   // std::time (daily rollover check)
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
namespace hades {

static constexpr int kMaxSteps = 25;

namespace {
// Canonical map key for the staleness guard: lexical normalization only ("./x", "a/../x" -> "x").
// NOT realpath — symlink aliasing is a documented v1 gap, same as the capability model's canon_path.
std::string canon_file_key(const std::string& p) {
  return std::filesystem::path(p).lexically_normal().string();
}
}  // namespace

// Push a message onto the in-memory conversation AND, when a session path is set, durably
// append it as one JSON line. IO errors are swallowed (a disk hiccup must not crash the turn —
// the message still lives in history_; resume just won't have the latest line). With no path
// set this is pure push_back, identical to the pre-resume behavior.
void Arbiter::append_history(const nlohmann::json& msg) {
  history_.push_back(msg);
  if (session_path_.empty()) return;
  std::error_code ec;
  const std::filesystem::path p(session_path_);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(session_path_, std::ios::app);
  // dump(indent=-1, indent_char=' ', ensure_ascii=false, error_handler=replace): invalid UTF-8
  // bytes in content become U+FFFD instead of throwing json::type_error.316 (which would unwind
  // through Blackboard::pump() and abort the turn). Compact one-line output for valid UTF-8.
  if (f) f << msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
}

// Reload a session jsonl into history_ on resume. Tolerant like load_memories: a blank or
// corrupt line (e.g. a truncated trailing line from a mid-append crash) is skipped, never thrown.
// window_start_() assumes well-formed history; load_history therefore sanitizes BOTH boundary
// orphans from a mid-pair crash (invalid to providers): a LEADING {role:tool} (a tool result whose
// owning assistant tool_calls was lost) and a TRAILING {role:assistant, tool_calls} (an assistant
// tool-call whose following tool result was lost) — so history_ opens AND closes on a clean
// user/assistant-answer boundary.
void Arbiter::load_history() {
  if (session_path_.empty()) return;
  // Tolerant parse shared with the GET /history display reader (skip blank/corrupt/partial lines).
  auto loaded = read_session_jsonl(session_path_);
  history_.insert(history_.end(), loaded.begin(), loaded.end());
  // Drop leading orphan tool message(s); the window must begin on user/assistant (or be empty).
  while (!history_.empty() && history_.front().value("role", "") == "tool")
    history_.erase(history_.begin());
  // Mid-pair crash: a truncated/lost tool-result line can leave a trailing assistant(tool_calls)
  // (an assistant tool-call with no following tool result). The next user turn would form
  // [assistant(tool_calls), user], which providers reject — drop the trailing orphan.
  while (!history_.empty() &&
         history_.back().value("role", "") == "assistant" &&
         history_.back().contains("tool_calls"))
    history_.pop_back();
  // Compaction sidecar: the same-id summary reloads with the session. Tolerant — missing/
  // garbage file or an upto beyond the loaded history (corrupt pairing) -> no summary; the
  // next window drop regenerates it.
  const std::string sc = sidecar_path_for(session_path_);
  if (!sc.empty()) {
    std::ifstream f(sc);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      const SidecarSummary s = parse_summary_sidecar(ss.str());
      if (!s.text.empty() && s.upto <= history_.size()) {
        summary_text_ = s.text;
        summarized_upto_ = s.upto;
      }
    }
  }
}

void Arbiter::on_attach(Blackboard& bb) {
  // SAFETY: bb_ is non-owning; Arbiter outlives the Blackboard subscription
  // (Launcher clears modules before bb destruct).
  bb_ = &bb;
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (!e.value.is_string()) return;  // ignore malformed user input
    // A session is a DAY: check the day boundary HERE, at the head of a new user turn, rather
    // than in start_turn(). start_turn() also runs tool-loop continuations, where rotating would
    // wipe the running turn's own history mid-flight (leaving an assistant(tool_calls) orphan and
    // an empty request); and it runs AFTER the append below, so a rotation there would discard the
    // very message that started the turn. Consequence, as designed: a turn that starts at 03:59
    // and finishes at 04:01 completes in the old session — turns are never split.
    maybe_roll_day_();
    append_history({{"role", "user"}, {"content", e.value}});
    steps_ = 0;
    ++turn_epoch_;   // a new user turn: later LLM_RESPONSEs stamped with prior epochs are stale
    bb_->post("MODE", "EXECUTING", "arbiter");
    start_turn();
  });
  bb.subscribe("LLM_RESPONSE", [this](const Entry& e) { on_llm_response(e); });
  bb.subscribe("TOOL_RESULT", [this](const Entry& e) { on_tool_result(e); });
  // Compaction replies (opt-in Module = compactor). SESSION_SUMMARY applies the rolling summary
  // (guarded in on_session_summary); COMPACT_FAILED just re-arms detection so the next start_turn
  // re-fires with the grown span. Both no-op without the module (nothing posts them).
  bb.subscribe("SESSION_SUMMARY", [this](const Entry& e) { on_session_summary(e); });
  bb.subscribe("COMPACT_FAILED", [this](const Entry& e) {
    if (!e.value.is_object()) return;
    if (e.value.value("session", "") != session_stem(session_path_)) return;
    pending_compact_ = false;   // re-arm: next start_turn re-fires with the grown span
  });
  // Bridge peer state: PEER.<peer>.card (capabilities) and PEER.<peer>.fact.<k> (reports). Kept
  // in a local latest-value map and folded into the leading system message at turn start (the
  // SKILLS_ANNOUNCE pattern). Harmless when no bridge exists (nothing posts PEER.*).
  bb.subscribe("PEER.*", [this](const Entry& e) { peer_vars_[e.key] = e.value; });
  bb.subscribe("CONFIRM_RESPONSE", [this](const Entry& e) { on_confirm(e); });
  // A front-end abandons a turn (run_until timeout) by posting TURN_ABANDONED. Bumping the
  // epoch invalidates the abandoned turn's in-flight LLM_RESPONSE (dropped by on_llm_response's
  // freshness gate when it lands after this), and clearing the pending confirm prevents a
  // confirm-gated action from the abandoned turn surviving into the next one (same reset as
  // on_confirm). Tools are offloaded now (tool-offload), so a stale TOOL_RESULT CAN arrive after
  // abandonment: on_tool_result's epoch gate drops it, and the orphan pop below keeps the
  // history clean.
  bb.subscribe("TURN_ABANDONED", [this](const Entry&) {
    ++turn_epoch_;
    clear_pending();
    // Tool-offload: an abandoned turn can leave a trailing assistant(tool_calls) whose
    // TOOL_RESULT is dropped by the epoch gate above (or never arrives). A later request
    // would then carry the orphan pair-head — provider-invalid. Pop it (the same sanitize
    // load_history applies on resume; the on-disk session line stays, resume handles it).
    while (!history_.empty() &&
           history_.back().value("role", "") == "assistant" &&
           history_.back().contains("tool_calls"))
      history_.pop_back();
  });
  // `/new` (REPL) posts NEW_SESSION to start a FRESH session mid-run: drop the in-memory
  // conversation and rotate session_path_ to a brand-new file so subsequent appends land there,
  // leaving the prior session intact on disk. The id is the session's LOGICAL DAY, so `/new`
  // yields a same-day sibling (2026-09-06 -> 2026-09-06-1) and does NOT move session_day_ — the
  // next rollover still fires at the cutoff. The id generator stays injectable (test seam); with
  // no day set either (a bare test Arbiter) it falls back to today's logical date.
  bb.subscribe("NEW_SESSION", [this](const Entry&) {
    rotate_session_(id_gen_ ? id_gen_()
                            : (session_day_.empty() ? current_session_id(day_cutoff_hour_)
                                                    : session_day_));
  });
}

// Rotate onto a fresh session file. Shared by `/new` and the daily rollover so the two can never
// drift: same state reset, same collision-safe naming, same lock, same SESSION_ROTATED post.
// Bumping turn_epoch_ (like TURN_ABANDONED) makes a brand-new turn context: an in-flight
// LLM_RESPONSE stamped with the old epoch is dropped by the freshness gate instead of landing in
// the fresh session.
void Arbiter::rotate_session_(const std::string& id) {
  const std::string from = session_stem(session_path_);
  history_.clear();
  clear_pending();
  // Fresh session: drop the rolling summary state so a stale summary can't leak into it.
  summary_text_.clear();
  summarized_upto_ = 0;
  pending_compact_ = false;
  ++turn_epoch_;
  if (sessions_dir_.empty()) {
    // Nowhere to rotate to (test/no-dir path): clear session_path_ so post-rotation turns are
    // in-memory-only rather than silently appending to the OLD session file.
    session_path_.clear();
  } else {
    // Collision-safe (same as the boot resolve): an existing dir/<id>.jsonl means a session for
    // this id already exists — `/new` twice in a day, or a rollover onto a day some other process
    // already opened — so take the first free `-N` rather than merging two conversations that our
    // freshly-cleared history_ does not contain. Then CLAIM it with the same advisory lock the
    // boot path takes: OnCollision::Reuse means a path is not ours by construction, and the
    // exclusivity Task 1 established would silently lapse at the first rotation otherwise (after
    // a rollover the process would hold yesterday's lock and none on today's file).
    // ponytail: the previous file's lock is never released (lock_session_file keeps the fd for the
    // process lifetime by design), so a rotation leaks one fd and leaves the closed session
    // locked against an explicit `--resume <that id>` elsewhere. Bounded — one per rollover (a
    // day) or per `/new` (human-paced). Releasing it needs lock_session_file to hand back the fd.
    session_path_ = lock_session_file(unique_fresh_path(sessions_dir_, id), OnHeld::Divert);
  }
  // Both rotation triggers announce themselves on ONE key: the embedding module's live-session
  // exclusion (and any future session-path consumer) re-points from this, and doing it for only
  // one of the two would leave them inconsistent for no reason.
  bb_->post("SESSION_ROTATED",
            {{"from", from}, {"to", session_stem(session_path_)}, {"path", session_path_}},
            "arbiter");
}

// Lazy daily rollover: a session is a DAY, so a process running across the cutoff must move on to
// the new day's file. Compared against session_day_ (the base id), never against the file stem —
// after a `/new` the stem is "2026-09-06-1" but the day is still "2026-09-06". An unset day means
// nobody told us which day this session belongs to (a bare test Arbiter): adopt today's and never
// rotate on the first turn.
void Arbiter::maybe_roll_day_() {
  const std::string today =
      logical_date(clock_ ? clock_() : std::time(nullptr), day_cutoff_hour_);
  if (session_day_.empty() || today == session_day_) {
    session_day_ = today;
    return;
  }
  rotate_session_(today);
  session_day_ = today;
}

void Arbiter::clear_pending() {
  pending_ = nullptr;
  pending_msg_ = nullptr;
}

// Bound the per-turn LLM request to history_budget_chars_: return the most-recent suffix of
// history_ whose cumulative serialized size fits the budget. The FULL history stays in memory and
// on disk — only the request is trimmed (this also caps an otherwise-unbounded history_ in a long
// live session). Two invariants: (1) never send ZERO history when history exists — the single
// most-recent message is always included even if it alone exceeds the budget; (2) never begin the
// window on an orphaned {role:tool} message (a tool result with no preceding assistant tool_calls
// is invalid to most providers) — advance past any leading tool message(s) onto a user/assistant
// boundary. Sizing uses the same fail-soft dump as append_history so a bad-UTF-8 message can't
// throw here.
std::size_t Arbiter::window_start_() const {
  const std::size_t n = history_.size();
  if (n == 0) return 0;
  // Walk newest -> oldest, accumulating dump sizes; include while within budget. s is the window
  // start; it stays at n (the sentinel) until the first message is included, which guarantees the
  // most-recent message is always kept even if it alone exceeds the budget.
  std::size_t s = n;
  double total = 0.0;
  for (std::size_t i = n; i-- > 0;) {
    const double sz = static_cast<double>(
        history_[i].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size());
    if (s != n && total + sz > history_budget_chars_) break;  // adding this would overflow; stop
    total += sz;
    s = i;
  }
  // Never begin on an orphaned tool result (provider-invalid). Advance past leading
  // tool messages; if that consumes everything (tail is a tool), back up onto the
  // owning assistant(tool_calls) so the window is the valid [assistant(tc), tool] pair.
  while (s < n && history_[s].value("role", "") == "tool") ++s;
  if (s >= n) {
    s = n - 1;
    while (s > 0 && history_[s].value("role", "") == "tool") --s;
  }
  return s;
}

// Merge two "- bullet\n" lists into one, de-duplicating identical lines, keyword order first.
static std::string merge_memory_blocks(const std::string& keyword, const std::string& semantic) {
  std::vector<std::string> lines;
  auto add = [&](const std::string& blk) {
    std::size_t pos = 0;
    while (pos < blk.size()) {
      std::size_t nl = blk.find('\n', pos);
      std::string line = blk.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (!line.empty() && std::find(lines.begin(), lines.end(), line) == lines.end()) lines.push_back(line);
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
  };
  add(keyword);
  add(semantic);
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) { out += lines[i]; if (i + 1 < lines.size()) out += "\n"; }
  return out;
}

void Arbiter::start_turn() {
  nlohmann::json tools = nlohmann::json::array();
  for (auto& t : tools_)
    tools.push_back({{"name", t.name}, {"description", t.description}, {"schema", t.schema}});
  // Leading system message = static SOUL/USER prompt + the live core-memory layer, re-read
  // from disk every turn (so the agent's core_memory edits show up the same session). Built fresh
  // each turn; never stored in history_.
  nlohmann::json messages = nlohmann::json::array();
  std::string sys = system_prompt_;
  if (!memory_path_.empty()) {
    std::string core = read_memory_layer(memory_path_);
    if (!core.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += core;
    }
  }
  // Rolling session summary (compaction): the agent's own earlier conversation, compacted.
  // Conversational context outranks tooling -> it sits right after core memory.
  if (!summary_text_.empty()) {
    if (!sys.empty()) sys += "\n\n";
    sys += "Earlier in this session (compacted from turns no longer shown; may be stale — "
           "re-verify files/live state before relying on a past action's result):\n" +
           summary_text_;
  }
  // Skills roster: fold the SkillsModule's SKILLS_ANNOUNCE (latest-value; posted at attach,
  // refreshed after a successful save_skill) into the same leading system message. Key absent,
  // non-string, or empty -> no block (a roster without the skills module costs nothing).
  if (auto sk = bb_->get("SKILLS_ANNOUNCE"); sk && sk->value.is_string()) {
    const std::string ann = sk->value.get<std::string>();
    if (!ann.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += ann;
    }
  }
  // Task list: fold the todo file (whole-list, agent-curated via the todo tool) into the
  // same leading system message — the standing plan for multi-step work, re-read each
  // turn like core memory. Empty path (tool not rostered) or missing/empty file -> no
  // block, zero cost.
  if (!todo_path_.empty()) {
    std::ifstream tf(todo_path_);
    if (tf) {
      std::stringstream ts;
      ts << tf.rdbuf();
      std::string plan = ts.str();
      while (!plan.empty() && (plan.back() == '\n' || plan.back() == ' ')) plan.pop_back();
      if (!plan.empty()) {
        if (!sys.empty()) sys += "\n\n";
        sys += "Your task list (keep it current with the todo tool):\n" + plan;
      }
    }
  }
  // Background tasks: fold the ToolRunner's BG_TASKS block (latest-value; posted on task
  // start and completion) — running + finished background work, the cross-turn delivery
  // half of background:true. The block carries its own header. Rebuilt every start_turn
  // (tool-loop continuations included), so a completion landing mid-turn is visible on
  // the next LLM round-trip. Absent, non-string, or empty -> no block.
  if (auto bg = bb_->get("BG_TASKS"); bg && bg->value.is_string()) {
    const std::string tasks = bg->value.get<std::string>();
    if (!tasks.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += tasks;
    }
  }
  // Peer capability + report folds (bridge protocol). Two blocks from the PEER.* map: cards ->
  // delegation targets; facts -> peer reports (trust-labeled, re-verify). Empty -> no block.
  {
    std::string deleg, reports;
    for (const auto& [key, val] : peer_vars_) {
      if (key.size() > 5 && key.compare(key.size() - 5, 5, ".card") == 0 && val.is_object()) {
        std::string line = "\n- " + val.value("name", "?");
        if (val.contains("skills") && val["skills"].is_array() && !val["skills"].empty()) {
          line += " skills:[";
          bool first = true;
          for (const auto& sk : val["skills"]) {
            if (!sk.is_object()) continue;            // tolerate non-object skill entries (untrusted peer data)
            line += (first ? "" : ",") + sk.value("id", "");
            first = false;
          }
          line += "]";
        }
        if (val.contains("caps") && val["caps"].is_object())
          line += " caps:" + val["caps"].dump();
        deleg += line;
      } else if (key.find(".fact.") != std::string::npos && val.is_object()) {
        const std::string who = val.value("from", "?");
        const std::string lbl = val.value("trust", "trusted") == "untrusted"
                                    ? "unverified claim from " + who
                                    : who + " reports";
        reports += "\n- " + lbl + ": " + val.value("text", "");
      }
    }
    if (!deleg.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += "Peers you can delegate to (use ask_agent by advertised capability):" + deleg;
    }
    if (!reports.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += "Reported by peers (treat as claims, re-verify before acting):" + reports;
    }
  }
  if (!sys.empty())
    messages.push_back({{"role", "system"}, {"content", sys}});
  // Compaction detect: messages before the window start fall out of THIS request. Post at
  // most one in-flight COMPACT_REQUEST (the CompactorModule, if rostered, ALWAYS answers
  // with SESSION_SUMMARY or COMPACT_FAILED). Without the module nothing subscribes and the
  // single post is inert — today's behavior exactly.
  const std::size_t ws = window_start_();
  if (ws > summarized_upto_ && !pending_compact_) {
    std::vector<nlohmann::json> span(history_.begin() + static_cast<std::ptrdiff_t>(summarized_upto_),
                                     history_.begin() + static_cast<std::ptrdiff_t>(ws));
    bb_->post("COMPACT_REQUEST",
              {{"session", session_stem(session_path_)},
               {"upto", ws},
               {"span", digest_span(span)},
               {"current_summary", summary_text_}},
              "arbiter");
    pending_compact_ = true;
  }
  for (std::size_t i = ws; i < history_.size(); ++i) messages.push_back(history_[i]);

  // Inject retrieved memory as an ephemeral {role:system} block before the last user message, in TWO
  // labeled sub-blocks so the LLM treats it as its OWN recall: saved FACTS (reliable) vs past-SESSION
  // excerpts (its memory of earlier conversations with this user; may be stale). Both empty -> no block.
  // Recomputed each turn, never stored in history_.
  std::string kw, sem_facts, sem_convos;
  if (auto m = bb_->get("RETRIEVED_MEMORY"); m && m->value.is_string()) kw = m->value.get<std::string>();
  if (auto m = bb_->get("RETRIEVED_MEMORY_SEMANTIC"); m && m->value.is_string()) sem_facts = m->value.get<std::string>();
  if (auto m = bb_->get("RETRIEVED_SESSION_SEMANTIC"); m && m->value.is_string()) sem_convos = m->value.get<std::string>();
  std::string facts = merge_memory_blocks(kw, sem_facts);   // dedup keyword + semantic facts
  std::string content;
  if (!facts.empty())
    content += "Facts from your memory (you saved these earlier; treat as reliable):\n" + facts;
  if (!sem_convos.empty()) {
    if (!content.empty()) content += "\n\n";
    content +=
        "Excerpts from earlier sessions with this same user — your own memory of past conversations. "
        "Treat them as things you and the user already discussed (do NOT say this is a first exchange, "
        "and do NOT treat them as the user quoting you). They record what was SAID then and may be out "
        "of date — re-verify current state (files, live data, tool results) before asserting a past "
        "action's result still holds:\n" + sem_convos;
  }
  if (!content.empty()) {
    nlohmann::json block = {{"role", "system"}, {"content", content}};
    int last_user = -1;
    for (int i = 0; i < static_cast<int>(messages.size()); ++i)
      if (messages[i].value("role", "") == "user") last_user = i;
    if (last_user >= 0) messages.insert(messages.begin() + last_user, block);
  }

  // The epoch is carried unchanged through tool-loop continuations (start_turn is re-called from
  // on_tool_result WITHOUT bumping it), so a within-turn LLM round-trip keeps the current epoch.
  bb_->post("LLM_REQUEST",
            {{"messages", messages}, {"tools", tools}, {"model", model_}, {"epoch", turn_epoch_}},
            "arbiter");
}

void Arbiter::on_llm_response(const Entry& e) {
  const auto& v = e.value;
  if (!v.is_object()) return;  // malformed response: ignore, never throw
  // Freshness gate: a response stamped with a superseded turn epoch (e.g. a timed-out turn's
  // worker completing late) is dropped, never applied to the current turn. Tool-loop
  // continuations share the current turn's epoch and so are NOT dropped.
  const std::uint64_t ep = v.value("epoch", static_cast<std::uint64_t>(0));
  if (ep != turn_epoch_) {
    bb_->post("DROPPED_STALE_LLM_RESPONSE", {{"epoch", ep}, {"current", turn_epoch_}}, "arbiter");
    return;
  }
  Action act;
  nlohmann::json assistant_msg;
  if (v.contains("tool_call") && v["tool_call"].is_object()) {
    const auto& tc = v["tool_call"];
    act.kind = Action::Kind::ToolCall;
    act.tool = tc.value("name", "");
    act.args = tc.contains("arguments") && tc["arguments"].is_object()
                   ? tc["arguments"]
                   : nlohmann::json::object();
    act.tool_id = tc.value("id", "");
    assistant_msg = {
        {"role", "assistant"},
        {"content", nullptr},
        {"tool_calls",
         nlohmann::json::array(
             {{{"id", act.tool_id},
               {"type", "function"},
               {"function", {{"name", act.tool}, {"arguments", act.args.dump()}}}}})}};
  } else {
    act.kind = Action::Kind::Answer;
    act.text = v.value("text", "");
    assistant_msg = {{"role", "assistant"}, {"content", act.text}};
  }
  bb_->post("NEXT_ACTION",
            {{"kind", static_cast<int>(act.kind)}, {"tool", act.tool}, {"text", act.text}},
            "arbiter");
  dispatch_or_gate(act, assistant_msg);
}

void Arbiter::dispatch_or_gate(const Action& act_in, const nlohmann::json& assistant_msg) {
  Action act = act_in;
  // Staleness guard: expect_version is Arbiter-owned plumbing. Strip anything LLM-supplied
  // (a hallucinated token must never reach the tool), then inject the recorded version when
  // this file has been observed. No record -> no injection -> the tool behaves as before. Done
  // BEFORE the objective loop so the confirm path's pending_ snapshot also carries the injection.
  if (act.kind == Action::Kind::ToolCall &&
      (act.tool == "edit_file" || act.tool == "write_file") && act.args.is_object()) {
    act.args.erase("expect_version");
    if (auto p = act.args.find("path"); p != act.args.end() && p->is_string()) {
      auto it = file_versions_.find(canon_file_key(p->get<std::string>()));
      if (it != file_versions_.end()) act.args["expect_version"] = it->second;
    }
  }
  // Objectives are consulted in registration order; the first to demand a
  // confirm (needs_confirm) or hard-veto wins and short-circuits dispatch.
  for (auto& o : objectives_) {
    if (!o->active(*bb_)) continue;
    // FAIL CLOSED on any objective exception (e.g. a non-string LLM arg reaching a buggy
    // veto): treat it as a hard block rather than letting it unwind pump() and crash the bus.
    VetoResult v;
    try {
      v = o->veto(*bb_, act);
    } catch (const std::exception& ex) {
      v = VetoResult{true, std::string("objective error: ") + ex.what(), false};
    } catch (...) {
      v = VetoResult{true, "objective error: unknown exception", false};
    }
    if (v.vetoed && v.needs_confirm) {
      pending_ = nlohmann::json{{"kind", static_cast<int>(act.kind)},
                                {"tool", act.tool},
                                {"args", act.args},
                                {"tool_id", act.tool_id}};
      pending_msg_ = assistant_msg;
      bb_->post("CONFIRM_REQUEST",
                {{"id", act.tool_id}, {"prompt", v.reason}, {"action", pending_}}, "arbiter");
      return;
    }
    if (v.vetoed) {
      bb_->post("ASSISTANT_MESSAGE", "[blocked: " + v.reason + "]", "arbiter");
      return;
    }
  }
  append_history(assistant_msg);
  if (act.kind == Action::Kind::ToolCall) {
    track_file_op_(act.tool_id, act.tool, act.args);
    bb_->post("TOOL_REQUEST",
              {{"id", act.tool_id}, {"tool", act.tool}, {"args", act.args},
               {"epoch", turn_epoch_}},
              "arbiter");
  } else {
    bb_->post("ASSISTANT_MESSAGE", act.text, "arbiter");
  }
}

// Record a tracked file op (fs_read/edit_file/write_file) keyed by tool-call id so its result's
// version can be harvested in on_tool_result. Only the three tools that report a version are tracked.
void Arbiter::track_file_op_(const std::string& id, const std::string& tool,
                             const nlohmann::json& args) {
  if (tool != "fs_read" && tool != "edit_file" && tool != "write_file") return;
  if (id.empty() || !args.is_object()) return;
  if (auto p = args.find("path"); p != args.end() && p->is_string())
    pending_file_ops_[id] = canon_file_key(p->get<std::string>());
}

void Arbiter::on_tool_result(const Entry& e) {
  const auto& v = e.value;
  if (!v.is_object()) return;  // malformed tool result: ignore
  const auto content = v.contains("content") ? v["content"] : nlohmann::json::object();
  // Staleness guard: a successful tracked file op reports the file's new content version. Erase the
  // pending entry either way (a failed/refused result must NOT update the map — it's still stale).
  if (auto it = pending_file_ops_.find(v.value("id", "")); it != pending_file_ops_.end()) {
    if (v.value("ok", false) && content.is_object())
      if (auto ver = content.find("version"); ver != content.end() && ver->is_string())
        file_versions_[it->second] = ver->get<std::string>();
    pending_file_ops_.erase(it);
  }
  // Freshness gate (tool-offload): a result stamped with a superseded epoch — its turn was
  // abandoned or rotated while the offloaded worker ran — must not continue the CURRENT
  // turn. The version harvest above already ran: a stale-but-successful write DID change
  // the disk, so its version is truth. A result with NO epoch field is NOT gated
  // (hand-posted/legacy producers).
  if (v.contains("epoch") &&
      (v["epoch"].is_number_integer() || v["epoch"].is_number_unsigned()) &&
      v["epoch"].get<std::uint64_t>() != turn_epoch_) {
    bb_->post("DROPPED_STALE_TOOL_RESULT",
              {{"epoch", v["epoch"]}, {"current", turn_epoch_}}, "arbiter");
    return;
  }
  // History push BEFORE the guard so the assistant/tool message pair is always preserved.
  append_history({{"role", "tool"},
                  {"tool_call_id", v.value("id", "")},
                  {"content", content.dump()}});
  if (++steps_ > kMaxSteps) {
    bb_->post("ASSISTANT_MESSAGE", "[stopped: reached max tool steps]", "arbiter");
    return;
  }
  start_turn();  // feed the result back to the LLM, continuing the turn
}

void Arbiter::on_confirm(const Entry& e) {
  if (pending_.is_null()) return;  // no action awaiting confirmation
  if (pending_.value("tool_id","") != e.value.value("id","")) return;  // stale/mismatched confirm
  const auto& v = e.value;
  bool approved = v.is_object() && v.value("approved", false);
  if (approved) {
    append_history(pending_msg_);
    // pending_["args"] already carries the staleness-guard injection (done before the veto loop).
    track_file_op_(pending_.value("tool_id", ""), pending_.value("tool", ""),
                   pending_.contains("args") ? pending_["args"] : nlohmann::json::object());
    bb_->post("TOOL_REQUEST",
              {{"id", pending_.value("tool_id", "")},
               {"tool", pending_.value("tool", "")},
               {"args", pending_.contains("args") ? pending_["args"] : nlohmann::json::object()},
               {"epoch", turn_epoch_}},
              "arbiter");
  } else {
    bb_->post("ASSISTANT_MESSAGE", "[declined by user]", "arbiter");
  }
  clear_pending();
}

// Apply a compactor reply. Guards: the session id must match the CURRENT session (a late
// worker from before a /new rotation must not contaminate the fresh session) and upto must
// advance monotonically within the loaded history. The sidecar write is atomic tmp+rename,
// best-effort (an IO failure loses persistence, never the in-memory summary).
void Arbiter::on_session_summary(const Entry& e) {
  const auto& v = e.value;
  if (!v.is_object()) return;
  if (v.value("session", "") != session_stem(session_path_)) return;
  pending_compact_ = false;
  const std::size_t upto =
      static_cast<std::size_t>(v.value("upto", static_cast<std::uint64_t>(0)));
  if (upto <= summarized_upto_ || upto > history_.size()) return;
  if (!v.contains("text") || !v["text"].is_string()) return;
  const std::string text = v["text"].get<std::string>();
  // Blank includes whitespace-only (review I1 defense-in-depth with the module's own gate):
  // adopting a blank summary would advance summarized_upto_ and drop real turns behind it.
  if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
  summary_text_ = text;
  summarized_upto_ = upto;
  const std::string sc = sidecar_path_for(session_path_);
  if (!sc.empty()) {
    // Parent dir created best-effort (spec; in practice append_history made it long ago).
    const std::filesystem::path scp(sc);
    if (scp.has_parent_path()) {
      std::error_code dec;
      std::filesystem::create_directories(scp.parent_path(), dec);
    }
    const std::string tmp = sc + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (f) {
      f << serialize_summary_sidecar(summarized_upto_, summary_text_);
      f.close();
    }
    if (f) {
      std::error_code ec;
      std::filesystem::rename(tmp, sc, ec);
      if (ec) std::remove(tmp.c_str());
    } else {
      std::remove(tmp.c_str());
    }
  }
  bb_->post("COMPACTED",
            {{"upto", static_cast<std::uint64_t>(summarized_upto_)},
             {"chars", static_cast<std::uint64_t>(summary_text_.size())}},
            "arbiter");
}

}  // namespace hades
