import asyncio
import json
import logging
import time

from dotenv import load_dotenv

load_dotenv()

from fastapi import FastAPI, WebSocket, WebSocketDisconnect

from mcp_client import McpClient
from opus_utils import decode_opus_frames, encode_pcm_to_opus
from asr import transcribe
from vad import detect_end_of_speech
from llm import generate_reply
from tts import synthesize

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

app = FastAPI()

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    logger.info("Client connected")
    
    mcp_client = McpClient()
    
    # Session state
    conversation_history = []
    audio_buffer = bytearray()
    wake_ts_us = 0
    in_stream = False
    cached_tools = []
    
    try:
        # Handshake
        await websocket.send_text(json.dumps({"type": "hello"}))
        hello_msg = await websocket.receive_text()
        logger.info(f"Received from device: {hello_msg}")
        
        # Initialize MCP
        await mcp_client.initialize(websocket)
        cached_tools = await mcp_client.list_tools(websocket)
        logger.info(f"Cached tools: {len(cached_tools)}")
        
        while True:
            msg = await websocket.receive()
            
            if "text" in msg:
                data = json.loads(msg["text"])
                msg_type = data.get("type")
                
                if msg_type == "stream_start":
                    wake_ts_us = data.get("timestamp_us", time.time() * 1e6)
                    audio_buffer.clear()
                    in_stream = True
                    logger.info(f"Stream started at {wake_ts_us}")
                    
                elif msg_type == "stream_end":
                    in_stream = False
                    logger.info("Stream ended. Processing audio...")
                    t0 = time.time()
                    
                    pcm_audio = decode_opus_frames(bytes(audio_buffer))
                    # VAD endpointing (optional depending on client, can trim trailing silence)
                    # end_idx = detect_end_of_speech(pcm_audio, sample_rate=16000)
                    # pcm_audio = pcm_audio[:end_idx]
                    
                    t_asr_start = time.time()
                    transcript = transcribe(pcm_audio, 16000)
                    logger.info(f"Transcript (took {time.time() - t_asr_start:.2f}s): {transcript}")
                    
                    conversation_history.append({"role": "user", "content": transcript})
                    
                    t_llm_start = time.time()
                    reply_text, tool_calls = await generate_reply(transcript, conversation_history, cached_tools)
                    logger.info(f"LLM reply (took {time.time() - t_llm_start:.2f}s): {reply_text}")
                    
                    if tool_calls:
                        for call in tool_calls:
                            logger.info(f"Calling tool: {call['name']}")
                            res = await mcp_client.call_tool(websocket, call['name'], call['arguments'])
                            logger.info(f"Tool result: {res}")
                            # Could feed this back to LLM in a real implementation
                            
                    if reply_text:
                        conversation_history.append({"role": "assistant", "content": reply_text})
                        t_tts_start = time.time()
                        pcm_tts = synthesize(reply_text)
                        opus_frames = encode_pcm_to_opus(pcm_tts)
                        logger.info(f"TTS synth (took {time.time() - t_tts_start:.2f}s)")
                        
                        await websocket.send_text(json.dumps({"type": "tts_start"}))
                        for frame in opus_frames:
                            await websocket.send_bytes(frame)
                        await websocket.send_text(json.dumps({"type": "tts_end"}))
                        
                elif msg_type == "abort":
                    logger.info("Abort TTS/process requested")
                    
                elif msg_type == "mcp":
                    # Let the MCP client handle tool call responses
                    mcp_client.handle_response(data)
                    
            elif "bytes" in msg:
                if in_stream:
                    audio_buffer.extend(msg["bytes"])
                    
    except WebSocketDisconnect:
        logger.info("Client disconnected")
    except Exception as e:
        logger.error(f"Error in websocket loop: {e}")
