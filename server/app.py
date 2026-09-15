import asyncio
import json
import logging
import struct
import time

from dotenv import load_dotenv

load_dotenv()

from fastapi import FastAPI, WebSocket, WebSocketDisconnect

from mcp_client import McpClient
from opus_utils import OpusStreamDecoder, OpusStreamEncoder
from asr import transcribe
from llm import generate_reply
from tts import synthesize, split_sentences

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

app = FastAPI()

BINARY_HEADER = struct.Struct("<HHII")  # version, type, timestamp_ms, payload_size
BINARY_TYPE_MIC = 0
BINARY_TYPE_TTS = 1
MAX_TOOL_ROUNDS = 3

# Opus TTS frame duration and send pacing. The device buffers a bounded amount
# of audio, so blasting a whole reply at once overflows it. Send slightly
# faster than real-time so a small jitter buffer builds on the device.
OPUS_FRAME_MS = 60
TTS_PACE_S = (OPUS_FRAME_MS / 1000.0) * 0.8


async def stream_tts(ws, encoder: OpusStreamEncoder, pcm: bytes):
    """Encode PCM and stream it as paced Opus TTS frames."""
    for packet in encoder.encode_pcm(pcm):
        await ws.send_bytes(pack_audio_frame(packet, BINARY_TYPE_TTS))
        await asyncio.sleep(TTS_PACE_S)


# Currently connected device socket (single-device server), used by /debug/*.
active_ws: WebSocket | None = None
active_mcp: McpClient | None = None


def pack_audio_frame(payload: bytes, frame_type: int) -> bytes:
    ts = int(time.time() * 1000) & 0xFFFFFFFF
    return BINARY_HEADER.pack(1, frame_type, ts, len(payload)) + payload


@app.get("/debug/volume")
async def debug_volume(v: int = 40):
    """Set the device speaker volume via MCP (0-100)."""
    if active_ws is None or active_mcp is None:
        return {"status": "no device connected"}
    result = await active_mcp.call_tool(
        active_ws, "self.audio_speaker.set_volume", {"volume": v})
    return {"status": "sent", "volume": v, "result": result}


@app.get("/debug/wake")
async def debug_wake():
    """Trigger a wake event on the device (test hook)."""
    if active_ws is None:
        return {"status": "no device connected"}
    await active_ws.send_text(json.dumps({"type": "wake"}))
    return {"status": "sent"}


@app.get("/debug/tts")
async def debug_tts(text: str = "Hello, I am Voxie."):
    """Synthesize `text` and send it to the device (speaker test hook)."""
    if active_ws is None:
        return {"status": "no device connected"}
    enc = OpusStreamEncoder()
    pcm = synthesize(text)
    await active_ws.send_text(json.dumps(
        {"type": "tts", "state": "start", "text": text}))
    if pcm:
        await stream_tts(active_ws, enc, pcm)
    await active_ws.send_text(json.dumps({"type": "tts", "state": "end"}))
    return {"status": "sent", "pcm_bytes": len(pcm)}


@app.get("/debug/notify")
async def debug_notify(text: str = "Reminder: you have a meeting in 10 minutes."):
    """Push an async spoken notification to the device (idle path)."""
    if active_ws is None:
        return {"status": "no device connected"}
    enc = OpusStreamEncoder()
    pcm = synthesize(text)
    await active_ws.send_text(json.dumps(
        {"type": "notify", "state": "start", "text": text}))
    if pcm:
        await stream_tts(active_ws, enc, pcm)
    await active_ws.send_text(json.dumps({"type": "notify", "state": "end"}))
    return {"status": "sent", "pcm_bytes": len(pcm)}


@app.get("/debug/tone")
async def debug_tone(freq: int = 440, ms: int = 1000):
    """Ask the device to generate a local sine tone (tests I2S/amp only)."""
    if active_ws is None:
        return {"status": "no device connected"}
    await active_ws.send_text(json.dumps(
        {"type": "tone", "freq": freq, "ms": ms}))
    return {"status": "sent", "freq": freq, "ms": ms}


@app.get("/debug/tone-opus")
async def debug_tone_opus(freq: int = 440, ms: int = 1000):
    """Send a pure sine through the Opus TTS path (tests codec + transport)."""
    if active_ws is None:
        return {"status": "no device connected"}
    import numpy as np

    t = np.arange(int(24000 * ms / 1000)) / 24000.0
    pcm = (np.sin(2 * np.pi * freq * t) * 16000.0).astype("<i2").tobytes()

    enc = OpusStreamEncoder()
    await active_ws.send_text(json.dumps(
        {"type": "tts", "state": "start", "text": ""}))
    await stream_tts(active_ws, enc, pcm)
    await active_ws.send_text(json.dumps({"type": "tts", "state": "end"}))
    return {"status": "sent", "freq": freq, "ms": ms, "pcm_bytes": len(pcm)}


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    global active_ws
    await websocket.accept()
    active_ws = websocket
    logger.info(f"Client connected: {websocket.client}")

    global active_mcp
    mcp_client = McpClient()
    active_mcp = mcp_client
    decoder = OpusStreamDecoder()
    encoder = OpusStreamEncoder()
    history = []

    pcm_buffer = bytearray()
    in_stream = False
    hello_received = asyncio.Event()
    utterances: asyncio.Queue = asyncio.Queue()

    async def receiver():
        nonlocal in_stream
        while True:
            msg = await websocket.receive()
            if msg.get("type") == "websocket.disconnect":
                break

            if msg.get("text") is not None:
                try:
                    data = json.loads(msg["text"])
                except json.JSONDecodeError:
                    continue
                mtype = data.get("type")
                if mtype == "hello":
                    logger.info(f"Device hello: {msg['text']}")
                    hello_received.set()
                elif mtype == "stream_start":
                    pcm_buffer.clear()
                    decoder.reset()
                    in_stream = True
                    logger.info(f"Stream start (wake_ts_us={data.get('wake_ts_us')})")
                elif mtype == "stream_end":
                    in_stream = False
                    pcm = bytes(pcm_buffer)
                    pcm_buffer.clear()
                    logger.info(f"Stream end ({len(pcm)} PCM bytes)")
                    utterances.put_nowait(pcm)
                elif mtype == "abort":
                    logger.info("Abort requested (barge-in)")
                elif mtype == "mcp":
                    mcp_client.handle_message(data)

            elif msg.get("bytes") is not None:
                raw = msg["bytes"]
                if in_stream and len(raw) > BINARY_HEADER.size:
                    version, ftype, _ts, _size = BINARY_HEADER.unpack_from(raw, 0)
                    if version == 1 and ftype == BINARY_TYPE_MIC:
                        pcm = decoder.decode(raw[BINARY_HEADER.size:])
                        if pcm:
                            pcm_buffer.extend(pcm)

    recv_task = asyncio.create_task(receiver())

    try:
        await websocket.send_text(json.dumps({"type": "hello"}))
        await asyncio.wait_for(hello_received.wait(), timeout=10)

        # Give the device a moment to finish registering all its tools
        # (IoT tools register after built-in tools on the device side).
        await asyncio.sleep(0.3)

        await mcp_client.initialize(websocket)
        tools = await mcp_client.list_tools(websocket, retries=3)
        logger.info(f"Cached {len(tools)} MCP tools from device")

        while True:
            pcm = await utterances.get()
            asyncio.create_task(
                _process_utterance(websocket, mcp_client, encoder, history, pcm))

    except asyncio.TimeoutError:
        logger.warning("Timed out waiting for device hello")
    except WebSocketDisconnect:
        logger.info("Client disconnected")
    except Exception as e:
        logger.exception(f"Error in websocket loop: {e}")
    finally:
        recv_task.cancel()
        if active_ws is websocket:
            active_ws = None


async def _process_utterance(websocket, mcp_client, encoder, history, pcm):
    try:
        # Debug: keep the most recent utterance for inspection.
        try:
            import wave as _wave
            with _wave.open("/root/.cache/voxie_last_utterance.wav", "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(16000)
                wf.writeframes(pcm)
        except Exception:
            pass

        transcript = transcribe(pcm, 16000)
        logger.info(f"Transcript: {transcript!r}")
        if not transcript:
            return

        await websocket.send_text(json.dumps({"type": "stt", "text": transcript}))
        history.append({"role": "user", "content": transcript})

        # Lazily re-fetch tools if the cache is empty (e.g. initial fetch
        # timed out).  This is the safety net that prevents "0 tools" from
        # persisting for the entire session.
        await mcp_client.ensure_tools(websocket)

        reply_text = ""
        for _round in range(MAX_TOOL_ROUNDS):
            reply_text, tool_calls = await generate_reply(history, mcp_client.tools)
            if not tool_calls:
                break
            for call in tool_calls:
                logger.info(f"Tool call: {call['name']} {call['arguments']}")
                result = await mcp_client.call_tool(
                    websocket, call["name"], call["arguments"])
                logger.info(f"Tool result: {result}")
                history.append({
                    "role": "user",
                    "content": f"[tool {call['name']} returned] {json.dumps(result)}",
                })
            reply_text = ""

        if not reply_text:
            reply_text, _ = await generate_reply(history, mcp_client.tools)
        if not reply_text:
            reply_text = "Sorry, I didn't catch that."

        history.append({"role": "assistant", "content": reply_text})
        logger.info(f"Reply: {reply_text!r}")

        # --- Stream TTS sentence-by-sentence for low latency ---------------
        # Split reply into sentence-sized chunks and synthesize + send each
        # one immediately so playback begins while later sentences are still
        # being generated.  This is how XiaoZhi achieves near-zero perceived
        # latency — the user hears the first sentence within ~200ms of the
        # LLM finishing, instead of waiting for the entire reply to synthesize.
        sentences = split_sentences(reply_text)
        if not sentences:
            sentences = [reply_text]

        await websocket.send_text(json.dumps(
            {"type": "tts", "state": "start", "text": reply_text}))

        for sentence in sentences:
            pcm_tts = await asyncio.to_thread(synthesize, sentence)
            if pcm_tts:
                await stream_tts(websocket, encoder, pcm_tts)

        await websocket.send_text(json.dumps({"type": "tts", "state": "end"}))

    except Exception as e:
        logger.exception(f"Utterance processing failed: {e}")

