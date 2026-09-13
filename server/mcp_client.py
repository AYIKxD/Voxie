import json
import logging
import asyncio

logger = logging.getLogger(__name__)

class McpClient:
    def __init__(self):
        self.request_id = 0
        self.pending_requests = {}
        self.tools = []

    async def initialize(self, websocket):
        self.request_id += 1
        req_id = self.request_id
        
        msg = {
            "type": "mcp",
            "id": req_id,
            "method": "mcp:initialize",
            "params": {
                "clientInfo": {"name": "VoxieServer", "version": "1.0"}
            }
        }
        await websocket.send_text(json.dumps(msg))
        
        # In a real app, you would wait on an asyncio Event for the response ID
        logger.info("Sent mcp:initialize")

    async def list_tools(self, websocket):
        self.request_id += 1
        req_id = self.request_id
        
        msg = {
            "type": "mcp",
            "id": req_id,
            "method": "mcp:tools/list"
        }
        await websocket.send_text(json.dumps(msg))
        logger.info("Sent mcp:tools/list")
        
        # Simplified: returning empty list. In practice, wait for matching response.
        return self.tools

    async def call_tool(self, websocket, name: str, arguments: str):
        self.request_id += 1
        req_id = self.request_id
        
        try:
            args_dict = json.loads(arguments) if isinstance(arguments, str) else arguments
        except:
            args_dict = {}
            
        msg = {
            "type": "mcp",
            "id": req_id,
            "method": "mcp:tools/call",
            "params": {
                "name": name,
                "arguments": args_dict
            }
        }
        await websocket.send_text(json.dumps(msg))
        logger.info(f"Sent mcp:tools/call for {name}")
        return {"status": "pending"}

    def handle_response(self, data: dict):
        resp_id = data.get("id")
        if resp_id in self.pending_requests:
            logger.info(f"Received MCP response for id {resp_id}")
            # Set event for waiter...
