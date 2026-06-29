import { percent } from "../lib/format.js";
import { prefs } from "../preferences.js";

export function createOutput(slider, miniSlider, out, onCommit, miniTooltip) {
    let dirty = false;

    // Positions the floating percent tooltip above the mini-player's thumb,
    // since (unlike the Output card's slider) it has no inline <output>.
    function positionTooltip() {
        if (!miniTooltip || !miniSlider) return;
        const min = parseFloat(miniSlider.min);
        const max = parseFloat(miniSlider.max);
        const pct = (parseFloat(miniSlider.value) - min) / (max - min);
        const thumbSize = 10;
        const left = thumbSize / 2 + pct * (miniSlider.offsetWidth - thumbSize);
        miniTooltip.style.left = `${left}px`;
        miniTooltip.style.transform = "translateX(-50%)";
        miniTooltip.textContent = percent(parseFloat(miniSlider.value));
    }

    if (miniTooltip && miniSlider) {
        miniSlider.addEventListener("mouseenter", () => {
            positionTooltip();
            miniTooltip.classList.add("show");
        });
        miniSlider.addEventListener("mouseleave", () => miniTooltip.classList.remove("show"));
        miniSlider.addEventListener("focus", () => {
            positionTooltip();
            miniTooltip.classList.add("show");
        });
        miniSlider.addEventListener("blur", () => miniTooltip.classList.remove("show"));
    }

    // restore saved volume if exists
    const savedVol = prefs.volume.get();
    if (savedVol !== null) {
        slider.value = savedVol;
        if (miniSlider) miniSlider.value = savedVol;
    }
    out.value = percent(parseFloat(slider.value));

    async function commit(value) {
        // save volume for next time
        prefs.volume.set(value);
        try {
            await onCommit(parseFloat(value));
        } finally {
            setTimeout(() => {
                dirty = false;
            }, 400);
        }
    }

    slider.addEventListener("input", () => {
        dirty = true;
        out.value = percent(parseFloat(slider.value));
        if (miniSlider) miniSlider.value = slider.value;
    });

    slider.addEventListener("change", () => commit(slider.value));

    if (miniSlider) {
        miniSlider.addEventListener("input", () => {
            dirty = true;
            out.value = percent(parseFloat(miniSlider.value));
            slider.value = miniSlider.value;
            positionTooltip();
        });

        miniSlider.addEventListener("change", () => commit(miniSlider.value));
    }

    return function render(state) {
        if (dirty) return;
        const gain = state?.audio?.output_gain ?? 0;
        // only update slider if value differs significantly, to avoid interrupting user interaction
        if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
        if (miniSlider && Math.abs(parseFloat(miniSlider.value) - gain) > 0.005) miniSlider.value = gain;
        out.value = percent(gain);
    };
}