#include "hades/llm/openai_compat_provider.h"
namespace hades {
OpenAICompatProvider::OpenAICompatProvider(std::string e, std::string k, std::string m, HttpClient h)
  : endpoint_(std::move(e)), key_(std::move(k)), model_(std::move(m)), http_(std::move(h)) {}
nlohmann::json OpenAICompatProvider::build_body(const LlmRequest& req) const {
  nlohmann::json b;
  b["model"] = req.model.empty() ? model_ : req.model;
  b["messages"] = req.messages;
  if(!req.tools.empty()){
    b["tools"]=nlohmann::json::array();
    for(const auto& t: req.tools)
      b["tools"].push_back({{"type","function"},
        {"function",{{"name",t.name},{"description",t.description},{"parameters",t.schema}}}});
    b["tool_choice"]="auto";
  }
  return b;
}
LlmResponse OpenAICompatProvider::complete(const LlmRequest& req){
  auto body=build_body(req).dump();
  auto resp=http_(endpoint_+"/chat/completions",
                  {{"Authorization","Bearer "+key_},{"Content-Type","application/json"}}, body);
  LlmResponse out;
  auto j=nlohmann::json::parse(resp.body, nullptr, false);
  if(j.is_discarded() || !j.contains("choices") || !j["choices"].is_array()
     || j["choices"].empty() || !j["choices"][0].contains("message")){
    out.stop_reason="parse_error"; return out;
  }
  const auto& choice=j["choices"][0];
  const auto& msg=choice["message"];
  if(msg.contains("content") && msg["content"].is_string()) out.text=msg["content"];
  if(msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()){
    const auto& tc=msg["tool_calls"][0];
    if(tc.contains("function") && tc["function"].is_object()){
      const auto& fn=tc["function"];
      nlohmann::json args=nlohmann::json::parse(fn.value("arguments","{}"), nullptr, false);
      out.tool_call={{"id",tc.value("id","")},{"name",fn.value("name","")},
                     {"arguments", args.is_discarded()?nlohmann::json::object():args}};
    }
  }
  out.stop_reason=choice.value("finish_reason","");
  if(j.contains("usage") && j["usage"].is_object()){
    out.prompt_tokens=j["usage"].value("prompt_tokens",0);
    out.completion_tokens=j["usage"].value("completion_tokens",0);
  }
  return out;
}
}  // namespace hades
