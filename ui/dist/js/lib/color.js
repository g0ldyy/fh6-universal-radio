// WCAG relative luminance / contrast ratio helpers, used to keep a dynamic
// accent color (extracted from album art) readable against the current
// page background — mainly an issue in light theme, where a pale extracted
// color can become nearly invisible as link/focus-ring text.
function relativeLuminance([r, g, b]) {
    const toLinear = c => {
        c /= 255;
        return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * toLinear(r) + 0.7152 * toLinear(g) + 0.0722 * toLinear(b);
}

function contrastRatio(rgbA, rgbB) {
    const lA = relativeLuminance(rgbA);
    const lB = relativeLuminance(rgbB);
    const lighter = Math.max(lA, lB);
    const darker = Math.min(lA, lB);
    return (lighter + 0.05) / (darker + 0.05);
}

function rgbToHsl([r, g, b]) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b);
    let h = 0, s = 0;
    const l = (max + min) / 2;
    const d = max - min;
    if (d !== 0) {
        s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
        switch (max) {
            case r: h = ((g - b) / d + (g < b ? 6 : 0)); break;
            case g: h = ((b - r) / d + 2); break;
            default: h = ((r - g) / d + 4);
        }
        h /= 6;
    }
    return [h, s, l];
}

function hslToRgb([h, s, l]) {
    if (s === 0) {
        const v = Math.round(l * 255);
        return [v, v, v];
    }
    const hue2rgb = (p, q, t) => {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1 / 6) return p + (q - p) * 6 * t;
        if (t < 1 / 2) return q;
        if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
        return p;
    };
    const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const p = 2 * l - q;
    return [
        Math.round(hue2rgb(p, q, h + 1 / 3) * 255),
        Math.round(hue2rgb(p, q, h) * 255),
        Math.round(hue2rgb(p, q, h - 1 / 3) * 255),
    ];
}

// Non-text decorative threshold (WCAG AA, 3:1) would suffice for borders and
// glows alone, but --accent is also used as link/focus-ring text color —
// aim for the text threshold (4.5:1) so it stays readable everywhere it's used.
const MIN_CONTRAST = 4.5;
const MAX_STEPS = 25;
const STEP = 0.04;

/**
 * Nudges `rgb` towards black or white (whichever increases contrast),
 * preserving hue/saturation, until it reaches MIN_CONTRAST against `bgRgb`.
 * Returns `rgb` unchanged if it already clears the threshold.
 * @param {[number,number,number]} rgb
 * @param {[number,number,number]} bgRgb
 * @returns {[number,number,number]}
 */
export function ensureContrast(rgb, bgRgb) {
    if (contrastRatio(rgb, bgRgb) >= MIN_CONTRAST) return rgb;

    const pushTowardsWhite = relativeLuminance(bgRgb) < 0.5;
    const [h, s, startL] = rgbToHsl(rgb);
    let l = startL;

    for (let i = 0; i < MAX_STEPS; i++) {
        l = pushTowardsWhite ? Math.min(1, l + STEP) : Math.max(0, l - STEP);
        const candidate = hslToRgb([h, s, l]);
        if (contrastRatio(candidate, bgRgb) >= MIN_CONTRAST) return candidate;
        if (l <= 0 || l >= 1) break;
    }
    // Saturation alone couldn't reach the threshold (e.g. a near-grey
    // source color) — fall back to a strongly light/dark version of the hue.
    return hslToRgb([h, s, pushTowardsWhite ? 0.92 : 0.18]);
}

export function parseRgb(str) {
    const m = (str || "").match(/rgba?\((\d+),\s*(\d+),\s*(\d+)/);
    return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
}

// The body's computed background-color is always a fully-resolved rgb()
// string (unlike reading a --custom-property's raw token text), so it's the
// reliable way to know the actual current theme background from JS.
export function getPageBackgroundRgb() {
    return parseRgb(getComputedStyle(document.body).backgroundColor) || [16, 16, 18];
}
