"""Send lifecycle identifiers and the final reply to the local Codex bridge."""
import hashlib
import json
import os
import sys
from pathlib import Path
from urllib.error import URLError
from urllib.request import ProxyHandler, Request, build_opener


def event_port():
    configured = os.environ.get("CODEX_STATUS_EVENT_PORT")
    if configured:
        return int(configured)
    if sys.platform != "darwin":
        return 8766
    runtime = Path.home() / "Library/Application Support/LS26 Codex/bridge-runtime.json"
    try:
        state = json.loads(runtime.read_text())
    except FileNotFoundError:
        return 8766
    port, pid = state.get("event_port"), state.get("pid")
    if type(port) is not int or not 1 <= port <= 65535 or type(pid) is not int or pid <= 0:
        raise ValueError("invalid LS26 bridge state")
    os.kill(pid, 0)
    return port


def main():
    try:
        payload = json.load(sys.stdin)
        event = {key: payload.get(key) for key in ("hook_event_name", "session_id", "turn_id")}
        if event["hook_event_name"] == "PreToolUse" and payload.get("tool_name") == "mcp__ls26__ask_choice":
            inputs = payload.get("tool_input")
            if isinstance(inputs, dict):
                event["choice_args"] = inputs.get("arguments", inputs)
        if event["hook_event_name"] in {"PermissionRequest", "PostToolUse"}:
            tool = payload.get("tool_name")
            inputs = payload.get("tool_input")
            if isinstance(tool, str) and isinstance(inputs, dict):
                identity = ({"command": inputs["command"]} if tool == "Bash" and "command" in inputs
                            else {key: value for key, value in inputs.items()
                                  if key not in {"description", "justification", "sandbox_permissions"}})
                digest = hashlib.sha256(json.dumps([tool, identity], sort_keys=True).encode()).hexdigest()
                event["approval_key"] = digest
            if event["hook_event_name"] == "PermissionRequest" and isinstance(tool, str):
                event["approval_tool"] = tool[:100]
                reason = inputs.get("description") if isinstance(inputs, dict) else None
                if isinstance(reason, str):
                    event["approval_reason"] = reason[:120]
        if event["hook_event_name"] == "Stop":
            print("{}")
            reply = payload.get("last_assistant_message")
            if isinstance(reply, str):
                event["reply"] = reply[:8192]
        port = event_port()
        request = Request(
            f"http://127.0.0.1:{port}/event",
            data=json.dumps(event, ensure_ascii=False).encode(),
            headers={"Content-Type": "application/json"},
        )
        with build_opener(ProxyHandler({})).open(request, timeout=0.75):
            pass
    except (ValueError, AttributeError, URLError, OSError):
        # The status display is optional, including when the bridge is stopped.
        pass


if __name__ == "__main__":
    main()
