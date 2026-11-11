"""
Thesis IoT Server — Agent loop as a LangGraph StateGraph

This is the /agent/chat tool-use loop, ported from the hand-written
`while loop_count < MAX_AGENT_TURNS` in app/api/agent_routes.py. Behaviour is
deliberately unchanged: same tools (AGENT_TOOLS stays the single schema
source), same confirm-before-delete gate (it lives inside _tool_delete_*,
the graph only carries its refusal back to the model), same queued-vs-sent
honesty (inside each _tool_*), same per-call exception isolation, same
8-turn cap, same SSE events in the same order.

Shape:

    START ─▶ agent ──(tool calls?)──▶ tools ──(turns < MAX?)──▶ agent
                │ no                     │ no
                ▼                        ▼
               END                      END

  - agent: one model call. Appends the AIMessage, bumps `turns`.
  - tools: runs every tool call on the last AIMessage, appends ToolMessages.

SSE streaming: nodes never yield. They push items through the LangGraph
stream writer (`runtime.stream_writer`), and the route consumes
`agent_graph.astream(..., stream_mode="custom")`. Items are tuples:
  ("sse", <already-formatted SSE string>)  — forwarded to the browser as-is
  ("model_reply", <text>)                   — the model's own words: forwarded
                                              as a "reply" event AND collected
                                              into the persisted message text
The split exists because the capture pipelines also emit "reply" events, and
those were never part of the persisted text in the old loop either.

Why a custom tools node instead of langgraph.prebuilt.ToolNode: ToolNode
runs LangChain @tool objects and returns their output as the ToolMessage.
Here two tools (capture_now, capture_sequence) are long async generators that
stream progress/image events while they run, and every other tool must emit
tool_call/tool_result SSE events around it. That is easier to keep exact in
a 40-line node than to bend ToolNode around.
"""

import json
import time
from dataclasses import dataclass
from typing import Annotated, Any, Awaitable, Callable, Literal, TypedDict

from langchain_core.language_models import BaseChatModel
from langchain_core.messages import AIMessage, AnyMessage, ToolMessage
from langchain_core.runnables import Runnable
from langgraph.graph import END, START, StateGraph
from langgraph.graph.message import add_messages
from langgraph.runtime import Runtime

from app.benchmark import timing as _timing

MAX_AGENT_TURNS = 8


class ClientDisconnected(Exception):
    """Raised inside a node when the browser has gone away (Stop button).
    Ends the graph run before the next model call or tool dispatch."""


class AgentState(TypedDict):
    # add_messages is the reducer: a node returning {"messages": [m]} APPENDS
    # m instead of replacing the list.
    messages: Annotated[list[AnyMessage], add_messages]
    turns: int  # model calls made so far this chat turn


@dataclass
class AgentContext:
    """Per-request dependencies. Passed as `context=` to astream, read in
    nodes as `runtime.context` — not part of the state, never checkpointed."""
    model: Runnable | BaseChatModel  # already bound to the tool schema
    model_key: str
    session_id: int | None
    bench_task_id: str
    bench_run_id: str
    is_disconnected: Callable[[], Awaitable[bool]]


def _pending_tool_calls(msg: AnyMessage) -> list[dict]:
    """Tool calls on an AIMessage as {id, name, args}.

    LangChain moves calls whose arguments are not valid JSON into
    `invalid_tool_calls`. The old loop ran those with {} rather than
    dropping them, and OpenAI rejects a follow-up request in which an
    assistant tool call has no matching tool message, so they are run here
    the same way."""
    if not isinstance(msg, AIMessage):
        return []
    calls = [
        {"id": tc["id"], "name": tc["name"], "args": tc.get("args")}
        for tc in msg.tool_calls
    ]
    calls += [
        {"id": tc.get("id"), "name": tc.get("name"), "args": {}}
        for tc in msg.invalid_tool_calls
    ]
    for c in calls:
        if not isinstance(c["args"], dict):
            c["args"] = {}
    return calls


async def agent_node(state: AgentState, runtime: Runtime[AgentContext]) -> dict:
    ctx = runtime.context
    if await ctx.is_disconnected():
        raise ClientDisconnected()

    t_plan_start = time.time()
    reply = await ctx.model.ainvoke(state["messages"])
    turns = state["turns"] + 1

    if turns == 1:
        # Planning latency of the first (planning) turn, same row as the
        # capture_now task that may follow.
        await _timing.record(
            ctx.bench_task_id,
            run_id=ctx.bench_run_id,
            t_plan_start=t_plan_start,
            t_plan_end=time.time(),
            model_key=ctx.model_key,
        )

    if reply.text.strip():
        runtime.stream_writer(("model_reply", reply.text))

    return {"messages": [reply], "turns": turns}


def _reply_text(ev: str) -> str:
    """Text of a capture pipeline's own "reply" SSE event, if `ev` is one."""
    if '"event": "reply"' not in ev:
        return ""
    try:
        return json.loads(ev.split("data: ", 1)[1].strip()).get("text", "")
    except (IndexError, json.JSONDecodeError, AttributeError) as _e:
        print(f"[WARN] SSE reply-parse error: {_e}")
        return ""


async def tools_node(state: AgentState, runtime: Runtime[AgentContext]) -> dict:
    # Late import: agent_routes imports this module. Looking the functions up
    # on the module at call time also keeps patch("app.api.agent_routes.X")
    # in tests effective.
    from app.api import agent_routes as ar

    ctx = runtime.context
    write = runtime.stream_writer
    results: list[ToolMessage] = []

    for call in _pending_tool_calls(state["messages"][-1]):
        if await ctx.is_disconnected():
            raise ClientDisconnected()

        name, args, call_id = call["name"], call["args"], call["id"]

        if name in ("capture_now", "capture_sequence"):
            # The pipelines emit their own tool_call/tool_result/image events.
            if name == "capture_now":
                pipeline = ar._capture_pipeline(
                    args, ctx.model_key,
                    bench_task_id=ctx.bench_task_id, bench_run_id=ctx.bench_run_id,
                )
                fallback = "Capture pipeline completed."
            else:
                pipeline = ar._capture_sequence_pipeline(
                    args, bench_run_id=ctx.bench_run_id, model_key=ctx.model_key,
                )
                fallback = "Sequence pipeline completed."
            pipeline_reply = ""
            async for ev in pipeline:
                write(("sse", ev))
                pipeline_reply = _reply_text(ev) or pipeline_reply
            results.append(ToolMessage(content=pipeline_reply or fallback, tool_call_id=call_id))
            continue

        write(("sse", ar._sse_event("tool_call", {
            "id": call_id,
            "label": ar._tool_label(name, args),
        })))

        try:
            result = await ar._execute_tool(name, args, ctx.session_id, model_key=ctx.model_key)
        except Exception as e:
            # Per-call isolation: one bad tool call must not kill the whole
            # streamed turn — feed the failure back to the LLM like any other
            # tool failure so it can adapt (retry, report, or move on).
            print(f"[AGENT] Tool '{name}' raised an unhandled exception: {e}")
            result = {"success": False, "summary": f"Tool '{name}' failed: {e}", "detail": ""}

        write(("sse", ar._sse_event("tool_result", {
            "id": call_id,
            "success": result["success"],
            "summary": result["summary"],
        })))
        results.append(ToolMessage(content=json.dumps(result), tool_call_id=call_id))

    return {"messages": results}


def route_after_agent(state: AgentState) -> Literal["tools", "__end__"]:
    return "tools" if _pending_tool_calls(state["messages"][-1]) else END


def route_after_tools(state: AgentState) -> Literal["agent", "__end__"]:
    # Same cap as the old loop: the tools of the 8th model call still run,
    # there is just no 9th call to read their results.
    return END if state["turns"] >= MAX_AGENT_TURNS else "agent"


def build_agent_graph():
    graph = StateGraph(AgentState, context_schema=AgentContext)
    graph.add_node("agent", agent_node)
    graph.add_node("tools", tools_node)
    graph.add_edge(START, "agent")
    graph.add_conditional_edges("agent", route_after_agent, ["tools", END])
    graph.add_conditional_edges("tools", route_after_tools, ["agent", END])
    return graph.compile()


# Compiled once: holds no per-request state (no checkpointer — chat history
# lives in the ChatMessage table, see _load_history/_persist_message).
agent_graph = build_agent_graph()

# Every super-step is one node run, so 2 per turn plus slack. The turn cap
# above is what actually stops the loop; this only guards against a bug in it.
RECURSION_LIMIT = 2 * MAX_AGENT_TURNS + 4


def initial_state(messages: list[AnyMessage]) -> dict[str, Any]:
    return {"messages": messages, "turns": 0}
