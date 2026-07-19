// tools/todo_main.cpp — bundled todo native tool binary
//
// Whole-list replace (the TodoWrite shape): reads one JSON line
// ({"call":"describe"|"todo","args":{items:[{text,status}]}}) and REWRITES the task-list
// file as markdown checkboxes — "- [ ]" pending, "- [~]" in_progress, "- [x]" done. The
// Arbiter folds this file into the leading system message every turn, so the list is the
// agent's standing plan across turns/sessions/heartbeat ticks. argv[1] = file path (fixed
// by wiring, never LLM-chosen; fallback ".hades/todo.md"). Caps refuse the WHOLE call (no
// partial write): >20 items, or an item text >200 bytes. An unknown non-empty status
// refuses the call too; absent/empty status = pending (empty-string-absent house rule).
// Empty items array = clear the list. Atomic write (temp+rename, parent dir created).
// Fail-closed: malformed input returns ok:false, never throws, never partial-writes.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

using nlohmann::json;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  const std::string file = argc > 1 ? argv[1] : ".hades/todo.md";
  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);

  json out;
  auto fail = [](const std::string& e) {
    return json{{"ok", false}, {"result", {{"error", e}}}};
  };
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "todo"},
             {"description",
              "REPLACE your whole task list (it is shown to you every turn). Send the "
              "full current list each time; statuses: pending | in_progress | done. Keep "
              "it current as you work and drop finished items that are no longer useful "
              "context; an empty items array clears the list. Max 20 items, 200 chars "
              "each."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"items",
                  {{"type", "array"},
                   {"items",
                    {{"type", "object"},
                     {"properties",
                      {{"text", {{"type", "string"}}},
                       {"status",
                        {{"type", "string"},
                         {"description",
                          "pending | in_progress | done (default pending)"}}}}},
                     {"required", {"text"}}}}}}}},
               {"required", {"items"}}}}}}};
  } else if (call == "todo") {
    json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                    ? in["args"]
                    : json::object();
    if (!args.contains("items") || !args["items"].is_array()) {
      out = fail("missing arg: items (array of {text,status}; empty array clears the list)");
    } else if (args["items"].size() > 20) {
      out = fail("too many items: " + std::to_string(args["items"].size()) +
                 "/20 — consolidate the list and resend");
    } else {
      constexpr std::size_t kTextCap = 200;
      std::string content;
      std::string err;
      for (const auto& it : args["items"]) {
        if (!it.is_object() || !it.contains("text") || !it["text"].is_string()) {
          err = "each item must be an object with a string text";
          break;
        }
        std::string text = it["text"].get<std::string>();
        for (char& c : text)
          if (c == '\n' || c == '\r') c = ' ';   // one item = one line
        if (text.empty()) {
          err = "item text must be non-empty";
          break;
        }
        if (text.size() > kTextCap) {
          err = "item text over " + std::to_string(kTextCap) + " chars — shorten it";
          break;
        }
        if (it.contains("status") && !it["status"].is_string()) {
          err = "status must be a string: pending | in_progress | done";   // fail closed
          break;
        }
        const std::string status =
            it.contains("status") ? it["status"].get<std::string>() : std::string{};
        std::string mark;
        if (status.empty() || status == "pending") mark = " ";
        else if (status == "in_progress") mark = "~";
        else if (status == "done") mark = "x";
        else {
          err = "unknown status '" + status + "' — use pending | in_progress | done";
          break;
        }
        content += "- [" + mark + "] " + text + "\n";
      }
      if (!err.empty()) {
        out = fail(err);
      } else {
        fs::path p(file);
        if (p.has_parent_path()) {
          std::error_code ec;
          fs::create_directories(p.parent_path(), ec);
        }
        const std::string tmp = file + ".tmp";
        std::ofstream f(tmp, std::ios::trunc);
        if (f) {
          f << content;
          f.close();
        }
        if (!f) {
          std::remove(tmp.c_str());
          out = fail("cannot write: " + file);
        } else {
          std::error_code ec;
          fs::rename(tmp, file, ec);   // atomic on POSIX; replaces existing
          if (ec) {
            std::remove(tmp.c_str());
            out = fail("cannot save: " + file);
          } else {
            out = {{"ok", true},
                   {"result",
                    {{"saved", true},
                     {"items", static_cast<int>(args["items"].size())}}}};
          }
        }
      }
    }
  } else {
    out = fail("unknown call: " + call);
  }

  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
            << std::endl;
  return 0;
}
