import { percent } from "../format.js";

export function createOutput(slider, out, onCommit) {
    let dirty = false;

    // restore saved volume if exists
    const savedVol = localStorage.getItem("fh6-volume");
    if (savedVol !== null) slider.value = savedVol;
    out.value = percent(parseFloat(slider.value));

    slider.addEventListener("input", () => {
        dirty = true;
        out.value = percent(parseFloat(slider.value));
    });

    slider.addEventListener("change", async () => {
        // save volume for next time
        localStorage.setItem("fh6-volume", slider.value);
        try {
            await onCommit(parseFloat(slider.value));
        } finally {
            setTimeout(() => {
                dirty = false;
            }, 400);
        }
    });

    return function render(state) {
        if (dirty) return;
        const gain = state?.audio?.output_gain ?? 0;
        // only update slider if value differs significantly, to avoid interrupting user interaction
        if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
        out.value = percent(gain);
    };
}