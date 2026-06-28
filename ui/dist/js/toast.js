const DURATION_MS = 5000;
const EXIT_MS = 200;

let stack = null;

function ensureStack() {
    if (stack) return stack;
    stack = document.createElement("div");
    stack.className = "toast-stack";
    document.body.appendChild(stack);
    return stack;
}

export function toast(message, isError = false) {
    const node = document.createElement("div");
    node.className = isError ? "toast err" : "toast";
    node.setAttribute("role", isError ? "alert" : "status");
    node.setAttribute("aria-live", isError ? "assertive" : "polite");
    node.textContent = message;
    ensureStack().appendChild(node);

    setTimeout(() => {
        node.classList.add("out");
        setTimeout(() => node.remove(), EXIT_MS);
    }, DURATION_MS);
}
