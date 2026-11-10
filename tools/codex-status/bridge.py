"""Relay Codex lifecycle events from loopback to an LS26 on the LAN."""
import argparse
import asyncio
import errno
import hmac
import json
import os
import re
import secrets
import socket
import tempfile
import time
from pathlib import Path

from aiohttp import web

from choice import ChoiceTarget, choice_args, handle_mcp, selection
from discovery import Discovery, PORT as DISCOVERY_PORT
from speech import render_reply, summary_context
from voice_input import MAX_PCM_BYTES, THREAD_ID, SenseVoiceInput, VoiceInput

EVENT_STATES = {
    "UserPromptSubmit": "working",
    "PreToolUse": "working",
    "PostToolUse": "working",
}
TERMINAL = {"Stop", "Interrupt", "SessionEnd"}


def approval_notice(data):
    tool = data.get("approval_tool")
    reason = data.get("approval_reason")
    if isinstance(reason, str):
        reason = " ".join(reason.split()).strip("。.!！")
        # Descriptions are optional and may include shell syntax or sensitive locators.
        if (reason and len(reason) <= 80
                and re.fullmatch(r"[\w\s，、,.()（）-]+", reason)
                and not re.search(r"[A-Za-z0-9_-]{24,}", reason)):
            return f"Codex 请求确认。{reason}。请在电脑上处理。"
    action = {"Bash": "运行命令", "apply_patch": "修改文件"}.get(tool if isinstance(tool, str) else "")
    if action:
        return f"Codex 请求{action}，请在电脑上查看并确认。"
    return "Codex 有一项操作需要确认，请在电脑上查看。"


class Bridge:
    def __init__(self, token, agent_voice=False, voice_input=None):
        self.token = token
        self.agent_voice = agent_voice
        self.session = None
        self.turn = None
        self.state = "idle"
        self.devices = {}
        self.revision = 0
        self.clip = None
        self.lock = asyncio.Lock()
        self.voice_input = voice_input
        self.voice_target = None
        self.approvals = set()
        self.approval_clients = {}
        self.choice_offer = None
        self.choice = None
        self.setup_devices = {}
        self.test_waiter = None
        self.device_modes = {}
        self.mode_waiter = None
        self.device_seen = False
        self.codex_verified = False

    def current(self):
        return self.state

    async def event(self, request):
        if request.content_type != "application/json" or "Origin" in request.headers:
            raise web.HTTPUnsupportedMediaType()
        try:
            data = await request.json()
            event = data["hook_event_name"]
            session = data["session_id"]
            turn = data.get("turn_id")
            if (not isinstance(event, str) or not isinstance(session, str) or
                    not session or (turn is not None and not isinstance(turn, str))):
                raise ValueError()
        except (ValueError, KeyError, TypeError):
            raise web.HTTPBadRequest() from None

        async with self.lock:
            previous = self.current()
            new_approval = False
            key = data.get("approval_key")
            if not isinstance(key, str) or not re.fullmatch(r"[0-9a-f]{64}", key):
                key = None
            if event == "UserPromptSubmit":
                self.codex_verified = True
                self.cancel_choice()
                self.session, self.turn, self.state = session, turn, "working"
                self.approvals.clear()
            elif session != self.session or (self.turn and turn and self.turn != turn):
                return web.Response(status=204)
            elif event == "PermissionRequest":
                self.cancel_choice()
                self.turn = self.turn or turn
                new_approval = key not in self.approvals
                self.approvals.add(key)
                self.state = "approval"
            elif event in EVENT_STATES:
                self.turn = self.turn or turn
                if event == "PreToolUse" and "choice_args" in data:
                    self.choice_offer = (choice_args(data["choice_args"]), time.monotonic() + 12)
                if event == "PostToolUse":
                    self.approvals.discard(key)
                self.state = "approval" if self.approvals or self.choice else "working"
            elif event == "PreModelRequest" and self.approvals:
                self.approvals.clear()
                self.state = "working"
            elif event in TERMINAL:
                self.cancel_choice()
                self.session, self.turn, self.state = None, None, "idle"
                self.approvals.clear()

            if self.state != previous or event == "UserPromptSubmit" or new_approval:
                self.revision += 1
                state = "done" if event == "Stop" else self.state
                self.voice_target = None
                if self.voice_input and state == "done" and THREAD_ID.fullmatch(session):
                    self.voice_target = (self.revision, session, time.monotonic() + 600)
                devices = tuple(self.devices.items())
                if event == "UserPromptSubmit" and previous != "idle":
                    # A new turn must re-arm the board's one-shot working notice.
                    await self.broadcast("idle", devices)
                reply = data.get("reply")
                if state == "approval" and any(ws in self.approval_clients for ws, _ in devices):
                    asyncio.create_task(self.finish_approval(self.revision, approval_notice(data), devices))
                elif (state == "done" and isinstance(reply, str) and reply.strip()
                        and any(dynamic for _, dynamic in devices)):
                    asyncio.create_task(
                        self.finish_reply(self.revision, reply[:8192], devices)
                    )
                else:
                    await self.broadcast(state, devices)
        return web.Response(status=204)

    def cancel_choice(self):
        self.choice_offer = None
        if self.choice:
            answer = self.choice.answer
            if not answer.done():
                answer.set_result(None)
            self.choice = self.voice_target = None

    def check_choice_devices(self):
        if self.choice:
            answer, devices = self.choice.answer, self.choice.devices
            if not answer.done() and not any(self.approval_clients.get(ws) == "choice" for ws in devices):
                answer.set_result(None)

    async def ask_choice(self, args):
        if args is None:
            return {"status": "invalid_question"}
        question, *options = args
        spoken = "。".join([question, *(f"第{n}项，{option}" for n, option in zip("一二三", options)),
                           "请唤醒后说第几项"]) + "。"
        if len(spoken.encode()) > 420:
            return {"status": "invalid_question"}
        async with self.lock:
            offer = self.choice_offer
            if self.state != "working" or not offer or offer[0] != args or time.monotonic() >= offer[1]:
                return {"status": "no_matching_turn"}
            self.choice_offer = None
            devices = tuple((ws, mode) for ws, mode in self.devices.items()
                            if self.approval_clients.get(ws) == "choice")
            if not (self.voice_input and self.agent_voice and devices):
                return {"status": "device_unavailable"}
            self.revision += 1
            answer = asyncio.get_running_loop().create_future()
            target = ChoiceTarget(self.revision, self.session, time.monotonic() + 45, answer,
                                  tuple(options), frozenset(ws for ws, _ in devices))
            self.choice = self.voice_target = target
            self.state = "approval"
        try:
            await self.finish_approval(target.revision, spoken, devices)
            selected = await asyncio.wait_for(answer, max(0, target.deadline - time.monotonic()))
            if selected is None:
                return {"status": "cancelled"}
            return {"status": "answered", "index": selected, "option": options[selected - 1]}
        except asyncio.TimeoutError:
            return {"status": "timeout"}
        finally:
            async with self.lock:
                if self.choice is target:
                    self.cancel_choice()
                    self.revision += 1
                    self.state = "working"
                    await self.broadcast("working", tuple(self.devices.items()))

    async def mcp(self, request):
        return await handle_mcp(request, self.ask_choice)

    async def setup(self, request):
        if "Origin" in request.headers or request.host.split(":")[0] not in {"127.0.0.1", "localhost"}:
            raise web.HTTPForbidden()
        devices = [{"address": address,
                    "office": self.device_modes.get(ws, self.approval_clients.get(ws) == "choice")}
                   for ws, address in self.setup_devices.items() if not ws.closed]
        if request.method == "GET":
            target = self.voice_target
            valid = target is not None and time.monotonic() < target[2]
            office_clients = [ws for ws in self.setup_devices
                              if not ws.closed and self.device_modes.get(ws)]
            if not self.voice_input:
                readiness = "disabled"
            elif valid and target is self.choice:
                readiness = ("choice" if any(ws in target.devices and
                             self.approval_clients.get(ws) == "choice" for ws in office_clients)
                             and not target.answer.done() else "busy")
            elif valid:
                readiness = "ready"
            else:
                readiness = "busy" if self.state != "idle" else "no-target"
            return web.json_response({"devices": devices, "seen": self.device_seen,
                                      "voice_readiness": readiness, "codex_verified": self.codex_verified},
                                     headers={"Cache-Control": "no-store"})
        if not devices:
            raise web.HTTPServiceUnavailable(reason="Device disconnected")
        if len(devices) != 1 or self.test_waiter:
            raise web.HTTPConflict(reason="Select one device or wait for the previous test")
        ws = next(ws for ws in self.setup_devices if not ws.closed)
        reply = asyncio.get_running_loop().create_future()
        self.test_waiter = (ws, reply)
        try:
            await self.send(ws, "1 T test")
            result = await asyncio.wait_for(reply, 4)
            return web.json_response({"result": result})
        except asyncio.TimeoutError:
            raise web.HTTPGatewayTimeout(reason="Device did not acknowledge the test") from None
        finally:
            self.test_waiter = None

    async def set_mode(self, request):
        if "Origin" in request.headers or request.host.split(":")[0] not in {"127.0.0.1", "localhost"}:
            raise web.HTTPForbidden()
        try:
            office = (await request.json())["office"]
        except (ValueError, KeyError, TypeError):
            raise web.HTTPBadRequest() from None
        if not isinstance(office, bool):
            raise web.HTTPBadRequest()
        devices = [ws for ws in self.setup_devices if not ws.closed]
        if not devices:
            raise web.HTTPServiceUnavailable(reason="Device disconnected")
        if len(devices) != 1 or self.mode_waiter:
            raise web.HTTPConflict(reason="Select one device or wait for the previous request")
        ws = devices[0]
        if ws not in self.device_modes:
            raise web.HTTPNotImplemented(reason="Device firmware does not support mode changes")
        if self.device_modes[ws] != office:
            reply = asyncio.get_running_loop().create_future()
            self.mode_waiter = (ws, office, reply)
            try:
                await self.send(ws, "1 T office" if office else "1 T play")
                await asyncio.wait_for(reply, 4)
            except asyncio.TimeoutError:
                raise web.HTTPGatewayTimeout(reason="Device did not confirm the new mode") from None
            finally:
                self.mode_waiter = None
        return web.json_response({"office": office})

    async def finish_approval(self, revision, notice, devices):
        try:
            audio = await asyncio.wait_for(asyncio.to_thread(render_reply, notice), 2.0)
        except asyncio.TimeoutError:
            audio = None
        async with self.lock:
            if revision != self.revision:
                return
            clip_id = None
            if audio:
                clip_id = secrets.token_hex(16)
                self.clip = (clip_id, time.monotonic() + 30, audio)
            await self.broadcast("approval", devices, clip_id, summary_context(notice))

    async def finish_reply(self, revision, reply, devices):
        needs_local = any(mode and (mode != "agent" or not self.agent_voice) for _, mode in devices)
        audio = await asyncio.to_thread(render_reply, reply) if needs_local else None
        text = summary_context(reply) if self.agent_voice else ""
        async with self.lock:
            if revision != self.revision:
                return
            clip_id = None
            if audio:
                clip_id = secrets.token_hex(16)
                self.clip = (clip_id, time.monotonic() + 30, audio)
            await self.broadcast("done", devices, clip_id, text)

    async def broadcast(self, state, devices, clip_id=None, text=""):
        frames = []
        for ws, mode in devices:
            if ws not in self.devices:
                continue
            frame = f"1 E {state}"
            if state == "approval" and self.agent_voice and text and self.approval_clients.get(ws) in {"agent", "choice"}:
                frame += f" {clip_id or '0' * 32} {text}"
            elif state == "done" and mode == "agent" and self.agent_voice and text:
                frame += f" {'0' * 32} {text}"
            elif mode and clip_id and (state != "approval" or ws in self.approval_clients):
                frame += f" {clip_id}"
            frames.append(self.send(ws, frame))
        await asyncio.gather(*frames)

    async def send(self, ws, frame):
        try:
            await asyncio.wait_for(ws.send_str(frame), timeout=0.25)
        except (ConnectionError, asyncio.TimeoutError, RuntimeError):
            self.devices.pop(ws, None)
            await ws.close(drain=False)

    async def audio(self, request):
        clip = self.clip
        if (clip is None or time.monotonic() >= clip[1]
                or not hmac.compare_digest(request.match_info["id"].encode(), clip[0].encode())):
            raise web.HTTPNotFound()
        print("Served Codex reply audio", flush=True)
        return web.Response(body=clip[2], content_type="audio/mpeg",
                            headers={"Cache-Control": "no-store"})

    async def accept_voice(self, ws, target, pcm, timing=None):
        result = "asr-error"
        text = ""
        started = time.monotonic()
        transcribed = started
        try:
            text = await self.voice_input.transcribe(pcm, choice=target is self.choice)
            transcribed = time.monotonic()
            if not text:
                result = "empty"
            else:
                result = "wait-turn"
            if text and not ws.closed and target == self.voice_target and time.monotonic() < target[2]:
                if target is self.choice:
                    selected = selection(text, len(target.options))
                    if selected and not target.answer.done() and self.approval_clients.get(ws) == "choice":
                        target.answer.set_result(selected)
                        result = "answered"
                    else:
                        result = "retry"
                else:
                    result = "submit-error"
                    await self.voice_input.submit(target[1], text)
                    result = "queued"
        except (OSError, RuntimeError, ValueError, asyncio.TimeoutError):
            pass
        if timing:
            print(f"Codex voice timing ready_ms={timing[0]} upload_ms={timing[1]}", flush=True)
        print(f"Codex voice input {result} asr={transcribed - started:.2f}s "
              f"delivery={time.monotonic() - transcribed:.2f}s", flush=True)
        await self.send(ws, f"1 V {result}")

    async def device(self, request):
        token = request.match_info["token"].encode()
        if not hmac.compare_digest(token, self.token.encode()):
            raise web.HTTPNotFound()
        voice = self.voice_input if request.path.endswith("/agent") else None
        ws = web.WebSocketResponse(heartbeat=15, timeout=1, max_msg_size=2048 if voice else 128)
        async with self.lock:
            await ws.prepare(request)
            await ws.send_str(f"1 S {self.current()}")
            self.devices[ws] = request.path.rsplit("/", 1)[-1] if request.path.count("/") == 3 else None
        recording = None
        submission = None
        timing = None
        try:
            async for message in ws:
                if voice and message.type == web.WSMsgType.BINARY:
                    if recording:
                        target, nonce, deadline, pcm = recording
                        if len(pcm) + len(message.data) <= MAX_PCM_BYTES and time.monotonic() < deadline:
                            pcm.extend(message.data)
                        else:
                            recording = None
                elif voice and message.type == web.WSMsgType.TEXT and message.data.startswith("1 V "):
                    command = message.data[4:]
                    if command == "start":
                        timing = None
                        target = self.voice_target
                        choice = target is not None and target is self.choice
                        eligible = not choice or (ws in target.devices and self.approval_clients.get(ws) == "choice"
                                                  and not target.answer.done())
                        if target and eligible and time.monotonic() < target[2] and (not submission or submission.done()):
                            nonce = secrets.token_hex(16)
                            # Allow the bound agent to finish the listening cue before capture.
                            recording = (target, nonce, time.monotonic() + 30, bytearray())
                            ready = f"{'choice-ready' if choice else 'ready'} {nonce}"
                            proof = hmac.digest(voice.key, ready.encode("ascii"), "sha256").hex()
                            await self.send(ws, f"1 V {ready} {proof}")
                            print("Codex voice input ready", flush=True)
                        else:
                            result = "unavailable"
                            if not target or time.monotonic() >= target[2]:
                                result = "wait-turn" if self.state != "idle" else "no-target"
                            await self.send(ws, f"1 V {result}")
                            print(f"Codex voice input {result}", flush=True)
                    elif command.startswith("timing ") and recording:
                        match = re.fullmatch(r"timing ([0-9]{1,5}) ([0-9]{1,5})", command)
                        if match:
                            timing = tuple(map(int, match.groups()))
                    elif command == "cancel":
                        recording = None
                        if submission and not submission.done():
                            submission.cancel()
                    elif command.startswith("end ") and recording:
                        target, nonce, deadline, pcm = recording
                        recording = None
                        digest = hmac.new(voice.key, nonce.encode("ascii"), "sha256")
                        digest.update(pcm)
                        if (target == self.voice_target and time.monotonic() < min(target[2], deadline)
                                and 6400 <= len(pcm) <= MAX_PCM_BYTES and len(pcm) % 2 == 0
                                and hmac.compare_digest(digest.hexdigest().encode(), command[4:].encode())):
                            submission = asyncio.create_task(self.accept_voice(ws, target, pcm, timing))
                        else:
                            await self.send(ws, "1 V unavailable")
                            print("Codex voice input rejected recording", flush=True)
                elif message.type == web.WSMsgType.TEXT and re.fullmatch(
                    r"1 D (?:motion|agent) [a-z-]{1,32}", message.data
                ):
                    if message.data == "1 D agent setup-ready":
                        self.setup_devices[ws] = request.remote
                        self.device_seen = True
                    if message.data in {"1 D agent mode-office", "1 D agent mode-play"}:
                        office = message.data.endswith("office")
                        self.device_modes[ws] = office
                        if (self.mode_waiter and self.mode_waiter[:2] == (ws, office)
                                and not self.mode_waiter[2].done()):
                            self.mode_waiter[2].set_result(None)
                    if (message.data in {"1 D agent test-queued", "1 D agent test-light", "1 D agent test-busy"}
                            and self.test_waiter and self.test_waiter[0] is ws
                            and not self.test_waiter[1].done()):
                        self.test_waiter[1].set_result(message.data.rsplit(" ", 1)[-1])
                    if message.data in {"1 D agent approval-clips", "1 D agent approval-agent",
                                        "1 D agent approval-choice"}:
                        self.approval_clients[ws] = message.data.rsplit("-", 1)[-1]
                        self.check_choice_devices()
                    print(f"Codex board {message.data[4:]}", flush=True)
        finally:
            if submission and not submission.done():
                submission.cancel()
                await asyncio.gather(submission, return_exceptions=True)
            self.devices.pop(ws, None)
            self.setup_devices.pop(ws, None)
            self.device_modes.pop(ws, None)
            self.approval_clients.pop(ws, None)
            self.check_choice_devices()
        return ws


async def run(args, token):
    voice_input = None
    if args.voice_key_file:
        key = bytes.fromhex(Path(args.voice_key_file).read_text().strip())
        if len(key) != 32:
            raise ValueError("Voice input requires a 32-byte hex key")
        if args.sensevoice_dir:
            voice_input = SenseVoiceInput(key, args.sensevoice_dir, args.codex)
        elif args.whisper and args.whisper_model:
            voice_input = VoiceInput(key, args.whisper, args.whisper_model, args.codex)
        else:
            raise ValueError("Voice input requires a local recognition model")
        print("Office ASR:", "SenseVoice" if args.sensevoice_dir else "Whisper", flush=True)
    bridge = Bridge(token, args.agent_voice, voice_input)
    local = web.Application(client_max_size=65536)
    local.router.add_post("/event", bridge.event)
    local.router.add_post("/mcp", bridge.mcp)
    local.router.add_get("/setup", bridge.setup)
    local.router.add_post("/setup/test", bridge.setup)
    local.router.add_post("/setup/mode", bridge.set_mode)
    lan = web.Application()
    lan.router.add_get("/status/{token}", bridge.device)
    lan.router.add_get("/status/{token}/audio", bridge.device)
    lan.router.add_get("/status/{token}/agent", bridge.device)
    lan.router.add_get("/audio/{id}.mp3", bridge.audio)
    runners = [web.AppRunner(app, access_log=None, handler_cancellation=True) for app in (local, lan)]
    discovery = None
    runtime = args.runtime_file
    published = None
    try:
        local_runner, lan_runner = runners
        await lan_runner.setup()
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                listener.bind((args.bind, args.port))
            except OSError as error:
                if error.errno != errno.EADDRINUSE or not args.discover or args.port != 8765:
                    raise
                listener.bind((args.bind, 0))
            lan_port = listener.getsockname()[1]
            await web.SockSite(lan_runner, listener.dup()).start()
        await local_runner.setup()
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                listener.bind(("127.0.0.1", args.event_port))
            except OSError as error:
                if error.errno != errno.EADDRINUSE or not runtime or args.event_port != 8766:
                    raise
                listener.bind(("127.0.0.1", 0))
            event_port = listener.getsockname()[1]
            await web.SockSite(local_runner, listener.dup()).start()
        if args.discover:
            discovery, _ = await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: Discovery(token, lan_port, legacy=lan_port == args.port), local_addr=("0.0.0.0", DISCOVERY_PORT)
            )
        if runtime:
            runtime.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            state = json.dumps({"event_port": event_port, "lan_port": lan_port, "pid": os.getpid()})
            with tempfile.NamedTemporaryFile(mode="w", dir=runtime.parent, delete=False) as file:
                pending = Path(file.name)
                file.write(state)
            try:
                pending.replace(runtime)
                published = state
            finally:
                pending.unlink(missing_ok=True)
        print(f"Codex bridge listening on {args.bind}:{lan_port}, local port {event_port}", flush=True)
        await asyncio.Event().wait()
    finally:
        if published:
            try:
                if runtime.read_text() == published:
                    runtime.unlink()
            except FileNotFoundError:
                pass
        if discovery:
            discovery.close()
        await asyncio.gather(*(runner.cleanup() for runner in runners))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", required=True, help="Host LAN IP to bind")
    parser.add_argument("--discover", action="store_true", help="Answer paired LAN device discovery")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--event-port", type=int, default=8766)
    parser.add_argument("--runtime-file", type=Path,
                        default=(Path.home() / "Library/Application Support/LS26 Codex/bridge-runtime.json"
                                 if os.sys.platform == "darwin" else None))
    parser.add_argument("--agent-voice", action="store_true", help="Try the board-bound agent voice")
    parser.add_argument("--voice-key-file")
    parser.add_argument("--sensevoice-dir", type=Path)
    parser.add_argument("--whisper")
    parser.add_argument("--whisper-model")
    parser.add_argument("--codex", default="codex")
    args = parser.parse_args()
    token = os.environ.get("CODEX_STATUS_TOKEN", "")
    if not re.fullmatch(r"[a-zA-Z0-9_-]{16,128}", token):
        parser.error("Set CODEX_STATUS_TOKEN to a URL-safe token of 16–128 characters")
    try:
        asyncio.run(run(args, token))
    except KeyboardInterrupt:
        pass
