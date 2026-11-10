"""Local MCP endpoint and bounded voice choices."""
import asyncio
import json
import re
import unicodedata
from typing import NamedTuple

from aiohttp import web


class ChoiceTarget(NamedTuple):
    revision: int
    session: str
    deadline: float
    answer: asyncio.Future
    options: tuple
    devices: frozenset


TOOL = {
    "name": "ask_choice",
    "description": (
        "Ask the user one low-risk project question through the LS26 in office mode. "
        "Use 2 or 3 short Chinese options and wait for the selected one. "
        "Never use for permissions, secrets, payments, deletion, external sends, or other consequential actions. "
        "If no answer arrives, ask in the chat. Only use when the user wants to respond on the device."
    ),
    "inputSchema": {
        "type": "object",
        "properties": {
            "question": {"type": "string", "maxLength": 38},
            "options": {
                "type": "array", "minItems": 2, "maxItems": 3,
                "items": {"type": "string", "maxLength": 16},
            },
        },
        "required": ["question", "options"],
        "additionalProperties": False,
    },
    "annotations": {"readOnlyHint": True, "openWorldHint": False},
}


def choice_args(args):
    if not isinstance(args, dict):
        return None
    question, options = args.get("question"), args.get("options")
    if not isinstance(options, list) or not 2 <= len(options) <= 3:
        return None
    values = (question, *options)
    limits = (38, *(16 for _ in options))
    if any(not isinstance(v, str) or not 1 <= len(v.strip()) <= limit
           or any(unicodedata.category(c).startswith("C") for c in v)
           for v, limit in zip(values, limits)):
        return None
    normalized = tuple(v.strip() for v in values)
    if len(set(normalized[1:])) != len(options):
        return None
    return normalized


def selection(transcript, count):
    words = unicodedata.normalize("NFKC", transcript)
    words = re.sub(r"[\s，,。.!！?？、]+", "", words)
    match = re.fullmatch(r"(?:我)?(?:要|选|選|选择|選擇)?(?:第)?([一二三123])(?:项|項)?", words)
    if match:
        index = "一二三123".index(match.group(1)) % 3 + 1
        if index <= count:
            return index
    return None


async def handle_mcp(request, ask):
    # Accept only local native clients. DNS rebinding and browser requests fail closed.
    if "Origin" in request.headers or request.host.split(":")[0] not in {"127.0.0.1", "localhost"}:
        raise web.HTTPForbidden()
    try:
        msg = await request.json()
    except ValueError:
        raise web.HTTPBadRequest() from None
    if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
        raise web.HTTPBadRequest()
    method = msg.get("method")
    if "id" not in msg:
        return web.Response(status=202)
    envelope = {"jsonrpc": "2.0", "id": msg["id"]}
    params = msg.get("params", {})
    if not isinstance(params, dict):
        raise web.HTTPBadRequest()
    if method == "initialize":
        versions = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}
        version = params.get("protocolVersion")
        result = {"protocolVersion": version if isinstance(version, str) and version in versions else "2025-11-25",
                  "capabilities": {"tools": {}}, "serverInfo": {"name": "ls26", "version": "1.0.0"}}
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": [TOOL]}
    elif method == "tools/call" and params.get("name") == TOOL["name"]:
        answer = await ask(choice_args(params.get("arguments")))
        result = {"content": [{"type": "text", "text": json.dumps(answer, ensure_ascii=False)}],
                  "isError": answer["status"] != "answered"}
    else:
        return web.json_response({**envelope, "error": {"code": -32601, "message": "Method not found"}})
    return web.json_response({**envelope, "result": result})
