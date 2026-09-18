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
from llm import _tools_to_gemini

from google import genai
from google.genai import types

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
        try:
            await ws.send_bytes(pack_audio_frame(packet, BINARY_TYPE_TTS))
        except Exception as e:
            # Device disconnected or send failed; stop cleanly.
            logger.warning(f"TTS stream aborted: {e}")
            return
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
    decoder = OpusStreamDecoder(sample_rate=16000)
    encoder = OpusStreamEncoder()

    in_stream = False
    hello_received = asyncio.Event()

    try:
        await websocket.send_text(json.dumps({"type": "hello"}))
        await asyncio.wait_for(hello_received.wait(), timeout=10)

        # Give the device a moment to finish registering all its tools
        await asyncio.sleep(0.3)

        await mcp_client.initialize(websocket)
        tools = await mcp_client.list_tools(websocket, retries=3)
        logger.info(f"Cached {len(tools)} MCP tools from device")

        gemini_tools = _tools_to_gemini(mcp_client.tools)

        import os
        api_key = os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY")
        client = genai.Client(api_key=api_key)

        config = types.LiveConnectConfig(
            response_modalities=[types.LiveClientContentModality.AUDIO],
            system_instruction=types.Content(parts=[types.Part.from_text(
                "You are Voxie, a concise and helpful voice assistant running on an ESP32-S3 device. "
                "Keep spoken replies short (1-3 sentences). When the user asks you to control the device, call the appropriate tool."
            )]),
            tools=gemini_tools if gemini_tools else None,
        )

        audio_queue = asyncio.Queue()

        async def receiver():
            nonlocal in_stream
            while True:
                msg = await websocket.receive()
                if msg.get("type") == "websocket.disconnect":
                    audio_queue.put_nowait(None)
                    break

                if msg.get("text") is not None:
                    try:
                        data = json.loads(msg["text"])
                    except json.JSONDecodeError:
                        continue
                    mtype = data.get("type")
                    if mtype == "hello":
                        hello_received.set()
                    elif mtype == "stream_start":
                        decoder.reset()
                        in_stream = True
                        logger.info("Mic stream start")
                    elif mtype == "stream_end":
                        in_stream = False
                        logger.info("Mic stream end")
                    elif mtype == "mcp":
                        mcp_client.handle_message(data)

                elif msg.get("bytes") is not None:
                    raw = msg["bytes"]
                    if in_stream and len(raw) > BINARY_HEADER.size:
                        version, ftype, _ts, _size = BINARY_HEADER.unpack_from(raw, 0)
                        if version == 1 and ftype == BINARY_TYPE_MIC:
                            pcm = decoder.decode(raw[BINARY_HEADER.size:])
                            if pcm:
                                audio_queue.put_nowait(pcm)

        recv_task = asyncio.create_task(receiver())

        async with client.aio.live.connect(model="gemini-3.1-flash-live", config=config) as session:
            logger.info("Connected to Gemini Live API")

            async def send_to_gemini():
                while True:
                    pcm = await audio_queue.get()
                    if pcm is None:
                        break
                    
                    await session.send(input=types.LiveClientRealtimeInput(
                        media_chunks=[types.Blob(
                            data=pcm, 
                            mime_type="audio/pcm;rate=16000"
                        )]
                    ))

            async def receive_from_gemini():
                async for response in session.receive():
                    server_content = response.server_content
                    if server_content:
                        model_turn = server_content.model_turn
                        if model_turn:
                            for part in model_turn.parts:
                                if part.inline_data:
                                    out_pcm = part.inline_data.data
                                    # Tell device TTS is starting if we want to trigger display state, 
                                    # but we can just stream frames directly.
                                    await stream_tts(websocket, encoder, out_pcm)

                    if response.tool_call:
                        for fc in response.tool_call.function_calls:
                            logger.info(f"Gemini Live Tool call: {fc.name}")
                            args = fc.args if hasattr(fc, "args") else {}
                            if not isinstance(args, dict):
                                try:
                                    args = dict(args)
                                except:
                                    args = {}
                                    
                            result = await mcp_client.call_tool(websocket, fc.name, args)
                            logger.info(f"Tool result: {result}")
                            
                            # Ensure result is a dictionary or appropriate JSON object
                            if not isinstance(result, dict):
                                result = {"result": result}
                                
                            await session.send(input=types.LiveClientToolResponse(
                                function_responses=[types.FunctionResponse(
                                    id=fc.id,
                                    name=fc.name,
                                    response=result
                                )]
                            ))

            send_task = asyncio.create_task(send_to_gemini())
            receive_task = asyncio.create_task(receive_from_gemini())

            done, pending = await asyncio.wait(
                [recv_task, send_task, receive_task],
                return_when=asyncio.FIRST_COMPLETED
            )
            for t in pending:
                t.cancel()

    except asyncio.TimeoutError:
        logger.warning("Timed out waiting for device hello")
    except WebSocketDisconnect:
        logger.info("Client disconnected")
    except Exception as e:
        logger.exception(f"Error in websocket loop: {e}")
    finally:
        if active_ws is websocket:
            active_ws = None

