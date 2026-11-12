"use client";

import { useSearchParams } from "next/navigation";
import { Suspense, useEffect, useState } from "react";
import styles from "./page.module.css";

// Owner login. POSTs to FastAPI's /api/auth/login (server/app/auth.py),
// which sets the HttpOnly session cookie; proxy.ts lets the browser through
// once that cookie exists.

function safeNext(raw: string | null): string {
	// Only same-site paths: "/board/x" yes, "//evil.com" or "https://..." no.
	if (!raw || !raw.startsWith("/") || raw.startsWith("//")) return "/";
	return raw;
}

function LoginForm() {
	const next = safeNext(useSearchParams().get("next"));
	const [password, setPassword] = useState("");
	const [error, setError] = useState<string | null>(null);
	const [busy, setBusy] = useState(false);

	// Already logged in, or login is off on this server: go straight in.
	useEffect(() => {
		fetch("/api/auth/me")
			.then((r) => r.json())
			.then((me: { auth: boolean; owner: boolean }) => {
				if (!me.auth) {
					// Sets the placeholder cookie proxy.ts looks for.
					return fetch("/api/auth/login", { method: "POST" }).then(() =>
						window.location.replace(next),
					);
				}
				if (me.owner) window.location.replace(next);
			})
			.catch(() => {});
	}, [next]);

	async function submit(e: React.FormEvent) {
		e.preventDefault();
		setBusy(true);
		setError(null);
		try {
			const res = await fetch("/api/auth/login", {
				method: "POST",
				headers: { "Content-Type": "application/json" },
				body: JSON.stringify({ password }),
			});
			if (res.ok) {
				window.location.replace(next);
				return;
			}
			const body = await res.json().catch(() => ({}));
			setError(body.detail || `Login failed (${res.status})`);
		} catch {
			setError("Can't reach the server");
		} finally {
			setBusy(false);
		}
	}

	return (
		<form className={styles.card} onSubmit={submit}>
			<h1 className={styles.title}>Hesperus</h1>
			<p className={styles.subtitle}>Owner login</p>
			<input
				className={styles.input}
				type="password"
				autoComplete="current-password"
				placeholder="Password"
				value={password}
				onChange={(e) => setPassword(e.target.value)}
				// biome-ignore lint/a11y/noAutofocus: single-field login page, focusing it is the whole point
				autoFocus
			/>
			{error && <p className={styles.error}>{error}</p>}
			<button
				className={styles.button}
				type="submit"
				disabled={busy || !password}
			>
				{busy ? "Signing in…" : "Sign in"}
			</button>
		</form>
	);
}

export default function LoginPage() {
	return (
		<main className={styles.page}>
			<Suspense>
				<LoginForm />
			</Suspense>
		</main>
	);
}
