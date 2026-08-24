/**
 * Shared inline SVG icon set. Every icon-like glyph in this app used to be
 * a bare Unicode character (arrows, checkmarks, an X, a warning triangle)
 * rendered as plain text — inconsistent sizing/baseline across fonts and
 * platforms, and on some mobile browsers some of these glyphs render as
 * missing-character boxes or emoji instead of a clean icon. This file is
 * the single place new icons should be added, instead of another one-off
 * inline <svg> or Unicode character somewhere in a page component.
 *
 * All icons: 1.5-1.8 stroke width, currentColor, square viewBox, no fill
 * unless noted. Pass width/height via props to resize; default 16.
 */

interface IconProps {
	size?: number;
	className?: string;
}

export function ArrowLeftIcon({ size = 16, className }: IconProps) {
	return (
		<svg
			width={size}
			height={size}
			viewBox="0 0 16 16"
			fill="none"
			className={className}
			aria-hidden="true"
		>
			<path
				d="M13 8H3M3 8L7.5 3.5M3 8l4.5 4.5"
				stroke="currentColor"
				strokeWidth="1.6"
				strokeLinecap="round"
				strokeLinejoin="round"
			/>
		</svg>
	);
}

export function ArrowRightIcon({ size = 16, className }: IconProps) {
	return (
		<svg
			width={size}
			height={size}
			viewBox="0 0 16 16"
			fill="none"
			className={className}
			aria-hidden="true"
		>
			<path
				d="M3 8h10M9.5 3.5L13 8l-3.5 4.5"
				stroke="currentColor"
				strokeWidth="1.6"
				strokeLinecap="round"
				strokeLinejoin="round"
			/>
		</svg>
	);
}

export function CheckIcon({ size = 14, className }: IconProps) {
	return (
		<svg
			width={size}
			height={size}
			viewBox="0 0 14 14"
			fill="none"
			className={className}
			aria-hidden="true"
		>
			<path
				d="M2.5 7.2L5.6 10.3L11.5 3.8"
				stroke="currentColor"
				strokeWidth="1.8"
				strokeLinecap="round"
				strokeLinejoin="round"
			/>
		</svg>
	);
}

export function PlusIcon({ size = 14, className }: IconProps) {
	return (
		<svg
			width={size}
			height={size}
			viewBox="0 0 14 14"
			fill="none"
			className={className}
			aria-hidden="true"
		>
			<path
				d="M7 2.5v9M2.5 7h9"
				stroke="currentColor"
				strokeWidth="1.8"
				strokeLinecap="round"
			/>
		</svg>
	);
}

export function XIcon({ size = 14, className }: IconProps) {
	return (
		<svg
			width={size}
			height={size}
			viewBox="0 0 14 14"
			fill="none"
			className={className}
			aria-hidden="true"
		>
			<path
				d="M3.5 3.5l7 7M10.5 3.5l-7 7"
				stroke="currentColor"
				strokeWidth="1.8"
				strokeLinecap="round"
			/>
		</svg>
	);
}

export function CircleIcon({ size = 14, className }: IconProps) {
	return (
		<svg
			width={size}
			height={size}
			viewBox="0 0 14 14"
			fill="none"
			className={className}
			aria-hidden="true"
		>
			<circle cx="7" cy="7" r="4.5" stroke="currentColor" strokeWidth="1.4" />
		</svg>
	);
}

export function WarningIcon({ size = 14, className }: IconProps) {
	return (
		<svg
			width={size}
			height={size}
			viewBox="0 0 14 14"
			fill="none"
			className={className}
			aria-hidden="true"
		>
			<path
				d="M7 1.5L13 12.5H1L7 1.5Z"
				stroke="currentColor"
				strokeWidth="1.3"
				strokeLinejoin="round"
			/>
			<path
				d="M7 5.5V8.5"
				stroke="currentColor"
				strokeWidth="1.3"
				strokeLinecap="round"
			/>
			<circle cx="7" cy="10.6" r="0.7" fill="currentColor" stroke="none" />
		</svg>
	);
}
