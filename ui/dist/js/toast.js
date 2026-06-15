export function toast(message, isError = false) {
  const node = document.createElement("div");
  node.className = isError ? "toast err" : "toast";
  node.setAttribute("role", isError ? "alert" : "status");
  node.setAttribute("aria-live", isError ? "assertive" : "polite");
  node.textContent = message;
  document.body.appendChild(node);
  setTimeout(() => node.remove(), 3500);
}
