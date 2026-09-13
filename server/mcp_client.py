import asyncio
import json
import logging

logger = logging.getLogger(__name__)


class McpClient:
    """MCP client that talks JSON-RPC to the device's MCP server over the WS.

    Each request gets a unique id; the response resolves the matching Future
    from the WebSocket receive loop via `handle_message`.
    """

    def __init__(self):
        self.request_id = 0
        self.pending = {}
        self.tools = []

    async def _request(self, websocket, method: str, params: dict | None = None,
                       timeout: float = 8.0):
        self.request_id += 1
        req_id = self.request_id
        fut = asyncio.get_event_loop().create_future()
        self.pending[req_id] = fut

        msg = {"type": "mcp", "id": req_id, "method": method}
        if params is not None:
            msg["params"] = params
        await websocket.send_text(json.dumps(msg))
        logger.info(f"Sent MCP {method} (id={req_id})")

        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self.pending.pop(req_id, None)
            logger.warning(f"MCP {method} (id={req_id}) timed out")
            return None

    def handle_message(self, data: dict):
        """Resolve a pending request from an MCP response message."""
        req_id = data.get("id")
        fut = self.pending.pop(req_id, None)
        if fut is None or fut.done():
            return
        if "error" in data:
            fut.set_exception(
                RuntimeError(data["error"].get("message", "MCP error"))
            )
        else:
            fut.set_result(data.get("result"))

    async def initialize(self, websocket):
        return await self._request(
            websocket, "initialize",
            {"clientInfo": {"name": "VoxieServer", "version": "1.0"}})

    async def list_tools(self, websocket):
        tools = []
        cursor = None
        while True:
            params = {"cursor": cursor} if cursor else None
            result = await self._request(websocket, "tools/list", params)
            if not result:
                break
            tools.extend(result.get("tools", []))
            cursor = result.get("nextCursor")
            if not cursor:
                break
        self.tools = tools
        return tools

    async def call_tool(self, websocket, name: str, arguments):
        if isinstance(arguments, str):
            try:
                arguments = json.loads(arguments) if arguments else {}
            except json.JSONDecodeError:
                arguments = {}
        return await self._request(
            websocket, "tools/call", {"name": name, "arguments": arguments or {}})
