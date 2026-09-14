import asyncio
import json
import logging

logger = logging.getLogger(__name__)


class McpClient:
    """MCP client that talks JSON-RPC to the device's MCP server over the WS.

    Each request gets a unique id; the response resolves the matching Future
    from the WebSocket receive loop via `handle_message`.

    Tool caching: once tools are successfully fetched they are retained for the
    lifetime of this client.  If the initial `list_tools` call times out or
    returns an empty list the caller can retry — the cache is only *replaced*
    when a non-empty result is obtained.
    """

    def __init__(self):
        self.request_id = 0
        self.pending: dict[int, asyncio.Future] = {}
        self.tools: list[dict] = []
        self._initialized = False

    # ------------------------------------------------------------------
    # Low-level request / response plumbing
    # ------------------------------------------------------------------

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
        if req_id is None:
            logger.debug("MCP message without id, ignoring")
            return
        fut = self.pending.pop(req_id, None)
        if fut is None or fut.done():
            logger.debug(f"MCP response id={req_id} has no pending future")
            return
        if "error" in data:
            fut.set_exception(
                RuntimeError(data["error"].get("message", "MCP error"))
            )
        else:
            fut.set_result(data.get("result"))

    # ------------------------------------------------------------------
    # MCP protocol methods
    # ------------------------------------------------------------------

    async def initialize(self, websocket):
        result = await self._request(
            websocket, "initialize",
            {"clientInfo": {"name": "VoxieServer", "version": "1.0"}})
        if result is not None:
            self._initialized = True
            logger.info(f"MCP initialized: {result}")
        else:
            logger.warning("MCP initialize failed (timeout)")
        return result

    async def list_tools(self, websocket, *, retries: int = 2):
        """Fetch the full tool list from the device, with retry on failure.

        The result is cached in ``self.tools``.  If the fetch returns a
        non-empty list it replaces any previous cache.  An empty / failed
        response does **not** clear a previously-good cache — this prevents
        a transient timeout from wiping out a known-good tool set.
        """
        for attempt in range(1, retries + 1):
            fetched: list[dict] = []
            cursor = None
            ok = True
            while True:
                params = {"cursor": cursor} if cursor else None
                result = await self._request(websocket, "tools/list", params)
                if result is None:
                    logger.warning(
                        f"tools/list page timed out (attempt {attempt}/{retries})")
                    ok = False
                    break
                fetched.extend(result.get("tools", []))
                cursor = result.get("nextCursor")
                if not cursor:
                    break

            if ok and fetched:
                self.tools = fetched
                logger.info(f"Cached {len(self.tools)} MCP tools")
                return self.tools

            if attempt < retries:
                wait = 1.0 * attempt
                logger.info(
                    f"Retrying tools/list in {wait}s "
                    f"(got {len(fetched)} tools, ok={ok})")
                await asyncio.sleep(wait)

        # All retries exhausted — keep whatever we already had cached.
        if self.tools:
            logger.warning(
                f"tools/list failed after {retries} attempts; "
                f"keeping {len(self.tools)} previously-cached tools")
        else:
            logger.error(
                f"tools/list failed after {retries} attempts and no "
                "cached tools available — LLM will have 0 tools")
        return self.tools

    async def call_tool(self, websocket, name: str, arguments):
        if isinstance(arguments, str):
            try:
                arguments = json.loads(arguments) if arguments else {}
            except json.JSONDecodeError:
                arguments = {}
        return await self._request(
            websocket, "tools/call", {"name": name, "arguments": arguments or {}})

    async def ensure_tools(self, websocket):
        """Re-fetch tools if the cache is empty (lazy recovery)."""
        if not self.tools:
            logger.info("Tool cache empty — re-fetching from device")
            await self.list_tools(websocket, retries=1)
        return self.tools
