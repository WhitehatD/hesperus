const BACKEND = process.env.BACKEND_URL || "http://server:8000";

export async function GET(
	request: Request,
	{ params }: { params: Promise<{ id: string }> },
) {
	const { id } = await params;
	const res = await fetch(`${BACKEND}/api/agent/sessions/${id}/messages`, {
		headers: { cookie: request.headers.get("cookie") ?? "" },
	});
	return new Response(await res.text(), {
		status: res.status,
		headers: { "Content-Type": "application/json" },
	});
}

export async function DELETE(
	request: Request,
	{ params }: { params: Promise<{ id: string }> },
) {
	const { id } = await params;
	const res = await fetch(`${BACKEND}/api/agent/sessions/${id}/messages`, {
		method: "DELETE",
		headers: { cookie: request.headers.get("cookie") ?? "" },
	});
	return new Response(await res.text(), {
		status: res.status,
		headers: { "Content-Type": "application/json" },
	});
}
