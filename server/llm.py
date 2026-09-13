import os
import logging
import asyncio

logger = logging.getLogger(__name__)

LLM_PROVIDER = os.environ.get("LLM_PROVIDER", "openai").lower()

async def generate_reply(transcript: str, history: list, tools: list = None) -> tuple[str, list]:
    """
    Generates a reply using the configured LLM provider.
    Returns (reply_text, tool_calls).
    """
    logger.info(f"Using LLM provider: {LLM_PROVIDER}")
    
    if LLM_PROVIDER == "openai":
        return await _generate_openai(transcript, history, tools)
    elif LLM_PROVIDER == "anthropic":
        return await _generate_anthropic(transcript, history, tools)
    else:
        # Default mock for testing
        logger.warning("Using mock LLM provider")
        return f"I heard you say: {transcript}", []

async def _generate_openai(transcript: str, history: list, tools: list):
    import openai
    client = openai.AsyncOpenAI(api_key=os.environ.get("OPENAI_API_KEY"))
    
    messages = [{"role": "system", "content": "You are a helpful voice assistant."}]
    messages.extend(history)
    
    # Note: formatting MCP tools to OpenAI format should be done here
    # For now, we skip tool formatting in this snippet.
    
    response = await client.chat.completions.create(
        model="gpt-4o-mini",
        messages=messages
    )
    
    msg = response.choices[0].message
    reply = msg.content if msg.content else ""
    tool_calls = []
    
    if msg.tool_calls:
        for tc in msg.tool_calls:
            tool_calls.append({
                "name": tc.function.name,
                "arguments": tc.function.arguments
            })
            
    return reply, tool_calls

async def _generate_anthropic(transcript: str, history: list, tools: list):
    import anthropic
    client = anthropic.AsyncAnthropic(api_key=os.environ.get("ANTHROPIC_API_KEY"))
    
    # Simplified mapping
    system = "You are a helpful voice assistant."
    
    response = await client.messages.create(
        model="claude-3-haiku-20240307",
        max_tokens=1024,
        system=system,
        messages=history
    )
    
    reply = response.content[0].text if response.content else ""
    return reply, []
