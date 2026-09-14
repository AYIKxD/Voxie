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
from tts import synthesize

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

app = FastAPI()

BINARY_HEADER = struct.Struct("<HHII")  # version, type, timestamp_ms, payload_size
BINARY_TYPE_MIC = 0
BINARY_TYPE_TTS = 1
MAX_TOOL_ROUNDS = 3


def pack_audio_frame(payload: bytes, frame_type: int) -> bytes:
    ts = int(time.time() * 1000) & 0xFFFFFFFF
    return BINARY_HEADER.pack(1, frame_type, ts, len(payload)) + payload


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    logger.info(f"Client connected: {websocket.client}")

    mcp_client = McpClient()
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

        await mcp_client.initialize(websocket)
        tools = await mcp_client.list_tools(websocket)
        logger.info(f"Cached {len(tools)} MCP tools")

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

        pcm_tts = synthesize(reply_text)
        await websocket.send_text(json.dumps(
            {"type": "tts", "state": "start", "text": reply_text}))
        if pcm_tts:
            for packet in encoder.encode_pcm(pcm_tts):
                await websocket.send_bytes(pack_audio_frame(packet, BINARY_TYPE_TTS))
        await websocket.send_text(json.dumps({"type": "tts", "state": "end"}))

    except Exception as e:
        logger.exception(f"Utterance processing failed: {e}")
