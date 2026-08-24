import "./styles/tokens.css";
import "./styles/base.css";
import type { Metadata } from "next";

export const metadata: Metadata = {
	title: "Hesperus — Autonomous IoT Visual Monitoring",
	description: "Autonomous IoT Visual Monitoring System Dashboard",
};

export default function RootLayout({
	children,
}: {
	children: React.ReactNode;
}) {
	return (
		<html lang="en">
			<body>{children}</body>
		</html>
	);
}
