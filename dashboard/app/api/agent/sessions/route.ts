const BACKEND = process.env.BACKEND_URL || "http://server:8000";

export async function GET(request: Request) {
	const { searchParams } = new URL(request.url);
	const boardId = searchParams.get("board_id") || "stm32-iot-cam-01";

	const res = await fetch(`${BACKEND}/api/agent/sessions?board_id=${boardId}`, {
		headers: { cookie: request.headers.get("cookie") ?? "" },
	});
	return new Response(await res.text(), {
		status: res.status,
		headers: { "Content-Type": "application/json" },
	});
}

export async function POST(request: Request) {
	const body = await request.text();
	const res = await fetch(`${BACKEND}/api/agent/sessions`, {
		method: "POST",
		headers: {
			"Content-Type": "application/json",
			cookie: request.headers.get("cookie") ?? "",
		},
		body,
	});
	return new Response(await res.text(), {
		status: res.status,
		headers: { "Content-Type": "application/json" },
	});
}
