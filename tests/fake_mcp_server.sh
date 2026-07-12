#!/bin/sh
# tests/fake_mcp_server.sh — canned one-shot MCP stdio server for tests.
#
# Reads the whole JSON-RPC conversation from stdin (run_subprocess closes the child's stdin
# after writing — src/core/subprocess.cpp:75), then emits a canned initialize result followed
# by the reply matching the request method: tools/call -> $FAKE_MCP_CALL_REPLY, anything else
# (tools/list) -> $FAKE_MCP_LIST_REPLY. Env-driven so each test picks its own wire replies.
input=$(cat)
case "$input" in
  *tools/call*) reply="$FAKE_MCP_CALL_REPLY" ;;
  *)            reply="$FAKE_MCP_LIST_REPLY" ;;
esac
echo '{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05"}}'
printf '%s\n' "$reply"
