import os
import logging
import asyncio

from dotenv import load_dotenv

load_dotenv()

logger = logging.getLogger(__name__)

LLM_PROVIDER = os.environ.get("LLM_PROVIDER", "openai").lower()

# Lazily created, reused across requests (client construction adds latency).
_gemini_client = None

SYSTEM_PROMPT = (
    "You are Voxie, a concise and helpful voice assistant running on an ESP32-S3 "
    "device. Keep spoken replies short (1-3 sentences). When the user asks you to "
    "control the device (lamp, LEDs, volume, display), call the appropriate tool."
)


def _tools_to_gemini(tools):
    from google.genai import types

    decls = []
    for t in tools or []:
        schema = t.get("inputSchema") or {"type": "object", "properties": {}}
        decls.append(
            types.FunctionDeclaration(
                name=t.get("name", ""),
                description=t.get("description", ""),
                parameters=schema,
            )
        )
    return [types.Tool(function_declarations=decls)] if decls else None


def _tools_to_openai(tools):
    out = []
    for t in tools or []:
        out.append({
            "type": "function",
            "function": {
                "name": t.get("name", ""),
                "description": t.get("description", ""),
                "parameters": t.get("inputSchema") or {"type": "object", "properties": {}},
            },
        })
    return out or None


async def generate_reply(history: list, tools: list = None) -> tuple[str, list]:
    """Generate a reply from the conversation history.

    history: list of {"role": "user"|"assistant", "content": str}
    tools:   list of MCP tool definitions (name/description/inputSchema)
    Returns (reply_text, tool_calls) where tool_calls is a list of
    {"name": str, "arguments": dict}.
    """
    logger.info(f"LLM provider: {LLM_PROVIDER}, {len(tools or [])} tools available")

    if LLM_PROVIDER == "gemini":
        return await _generate_gemini(history, tools)
    if LLM_PROVIDER == "openai":
        return await _generate_openai(history, tools)
    if LLM_PROVIDER == "anthropic":
        return await _generate_anthropic(history, tools)

    logger.warning("Using mock LLM provider")
    last = history[-1]["content"] if history else ""
    return f"I heard you say: {last}", []


async def _generate_gemini(history: list, tools: list):
    from google import genai
    from google.genai import types

    api_key = os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY")
    if not api_key:
        logger.error("GEMINI_API_KEY is not set")
        return "Gemini API key is not configured.", []

    model_name = os.environ.get("GEMINI_MODEL", "gemini-3.1-flash-live")

    contents = []
    for m in history:
        role = "user" if m.get("role") == "user" else "model"
        contents.append(types.Content(role=role, parts=[types.Part(text=m.get("content", ""))]))

    config = types.GenerateContentConfig(system_instruction=SYSTEM_PROMPT)
    # Disable Gemini "thinking" and use plain Flash behaviour. On the 2.5
    # models thinking is on by default and adds several seconds before the
    # first token, which dominates our end-to-end latency.
    try:
        config.thinking_config = types.ThinkingConfig(thinking_budget=0)
    except Exception:
        logger.warning("thinking_config unsupported on this google-genai version")
    tool_decls = _tools_to_gemini(tools)
    if tool_decls:
        config.tools = tool_decls

    global _gemini_client
    if _gemini_client is None:
        _gemini_client = genai.Client(api_key=api_key)

    def _call():
        return _gemini_client.models.generate_content(
            model=model_name, contents=contents, config=config)

    try:
        response = await asyncio.to_thread(_call)
    except Exception as e:
        logger.error(f"Gemini request failed: {e}")
        return "Sorry, I couldn't reach the language model.", []

    reply = ""
    try:
        reply = (response.text or "").strip()
    except Exception:
        reply = ""

    tool_calls = []
    for fc in (getattr(response, "function_calls", None) or []):
        args = dict(fc.args) if getattr(fc, "args", None) else {}
        tool_calls.append({"name": fc.name, "arguments": args})

    return reply, tool_calls


async def _generate_openai(history: list, tools: list):
    import openai

    client = openai.AsyncOpenAI(api_key=os.environ.get("OPENAI_API_KEY"))
    messages = [{"role": "system", "content": SYSTEM_PROMPT}] + history

    kwargs = {"model": os.environ.get("OPENAI_MODEL", "gpt-4o-mini"), "messages": messages}
    tool_spec = _tools_to_openai(tools)
    if tool_spec:
        kwargs["tools"] = tool_spec

    response = await client.chat.completions.create(**kwargs)
    msg = response.choices[0].message
    reply = msg.content or ""
    tool_calls = []
    for tc in (msg.tool_calls or []):
        import json
        try:
            args = json.loads(tc.function.arguments or "{}")
        except json.JSONDecodeError:
            args = {}
        tool_calls.append({"name": tc.function.name, "arguments": args})
    return reply, tool_calls


async def _generate_anthropic(history: list, tools: list):
    import anthropic

    client = anthropic.AsyncAnthropic(api_key=os.environ.get("ANTHROPIC_API_KEY"))
    messages = [m for m in history if m.get("role") in ("user", "assistant")]

    kwargs = {
        "model": os.environ.get("ANTHROPIC_MODEL", "claude-3-5-haiku-latest"),
        "max_tokens": 512,
        "system": SYSTEM_PROMPT,
        "messages": messages,
    }
    if tools:
        kwargs["tools"] = [
            {
                "name": t.get("name", ""),
                "description": t.get("description", ""),
                "input_schema": t.get("inputSchema") or {"type": "object", "properties": {}},
            }
            for t in tools
        ]

    response = await client.messages.create(**kwargs)
    reply = ""
    tool_calls = []
    for block in response.content:
        if block.type == "text":
            reply += block.text
        elif block.type == "tool_use":
            tool_calls.append({"name": block.name, "arguments": block.input or {}})
    return reply, tool_calls
