import { el } from "../lib/dom.js";
import { t } from "../i18n.js";

// Generic single-field text modal — replaces window.prompt() so every part
// of the UI (station rename, etc.) shares the same look as the station
// editor modal in onlineRadio.js, instead of a native browser dialog.
let modal, titleEl, input, errorEl, saveBtn, cancelBtn, resolveFn;

function ensureModal() {
    if (modal) return;
    input = el("input", { type: "text" });
    titleEl = el("h3", {});
    errorEl = el("p", { class: "modal-error muted", hidden: true });
    saveBtn = el("button", { type: "button", class: "btn filled", dataset: { i18n: "btn.save" } }, t("btn.save"));
    cancelBtn = el("button", { type: "button", class: "btn ghost danger", dataset: { i18n: "btn.cancel" } }, t("btn.cancel"));

    modal = el("div", { class: "modal-overlay", hidden: true }, [
        el("div", { class: "modal-card" }, [
            el("div", { class: "modal-head" }, [titleEl]),
            input,
            errorEl,
            el("div", { class: "modal-foot end row" }, [cancelBtn, saveBtn]),
        ]),
    ]);
    document.body.append(modal);

    const close = value => {
        modal.hidden = true;
        document.body.style.overflow = "";
        resolveFn?.(value);
        resolveFn = null;
    };

    const submit = () => {
        const value = input.value.trim();
        if (!value) {
            errorEl.textContent = t("error.value_required");
            errorEl.hidden = false;
            return;
        }
        close(value);
    };

    cancelBtn.addEventListener("click", () => close(null));
    saveBtn.addEventListener("click", submit);
    modal.addEventListener("click", e => e.target === modal && close(null));
    input.addEventListener("keydown", e => {
        if (e.key === "Enter") submit();
        if (e.key === "Escape") close(null);
    });
}

/**
 * Single-field text prompt modal.
 * @param {{ title: string, value?: string }} opts
 * @returns {Promise<string|null>} the trimmed value, or null if cancelled
 */
export function promptInput({ title, value = "" } = {}) {
    ensureModal();
    titleEl.textContent = title;
    input.value = value;
    errorEl.hidden = true;
    return new Promise(resolve => {
        resolveFn = resolve;
        modal.hidden = false;
        document.body.style.overflow = "hidden";
        input.focus();
        input.select();
    });
}
