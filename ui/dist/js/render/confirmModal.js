import { el } from "../lib/dom.js";
import { t } from "../i18n.js";

// Generic confirmation modal — replaces window.confirm() for destructive
// actions (deleting a station), so it shares the same look as the other
// modals instead of a native browser dialog.
let modal, titleEl, messageEl, confirmBtn, cancelBtn, resolveFn;

function ensureModal() {
    if (modal) return;
    titleEl = el("h3", {});
    messageEl = el("p", { class: "muted" });
    confirmBtn = el("button", { type: "button", class: "btn ghost danger" });
    cancelBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "btn.cancel" } }, t("btn.cancel"));

    modal = el("div", { class: "modal-overlay", hidden: true }, [
        el("div", { class: "modal-card" }, [
            el("div", { class: "modal-head" }, [titleEl]),
            messageEl,
            el("div", { class: "modal-foot end row" }, [cancelBtn, confirmBtn]),
        ]),
    ]);
    document.body.append(modal);

    const close = value => {
        modal.hidden = true;
        document.body.style.overflow = "";
        resolveFn?.(value);
        resolveFn = null;
    };

    cancelBtn.addEventListener("click", () => close(false));
    confirmBtn.addEventListener("click", () => close(true));
    modal.addEventListener("click", e => e.target === modal && close(false));
    document.addEventListener("keydown", e => {
        if (!modal.hidden && e.key === "Escape") close(false);
    });
}

/**
 * Asks the user to confirm a destructive action.
 * @param {{ title: string, message: string, confirmLabel?: string }} opts
 * @returns {Promise<boolean>} true if confirmed, false if cancelled
 */
export function confirmAction({ title, message, confirmLabel = t("btn.delete") } = {}) {
    ensureModal();
    titleEl.textContent = title;
    messageEl.textContent = message;
    confirmBtn.textContent = confirmLabel;
    return new Promise(resolve => {
        resolveFn = resolve;
        modal.hidden = false;
        document.body.style.overflow = "hidden";
        cancelBtn.focus();
    });
}
