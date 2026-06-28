export const $ = (selector, root = document) => root.querySelector(selector);
export const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

// Only write when the value changes, to avoid cursor jumps in focused inputs.
export function setText(node, value) {
  const text = String(value);
  if (node && node.textContent !== text) node.textContent = text;
}

// Delays `fn` until `wait` ms have passed without another call — for
// input handlers that re-render a list on every keystroke.
export function debounce(fn, wait = 150) {
  let timer = null;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), wait);
  };
}

// Wraps case-insensitive substring matches of `terms` in <mark> elements, for
// visually highlighting search results. Matches the original (non-folded)
// text, so accented characters typed without their diacritic won't highlight
// — fine for a visual aid, since the actual filtering logic folds separately.
export function highlightText(text, terms) {
  const str = String(text ?? "");
  const pattern = (terms || [])
    .map(term => term.trim())
    .filter(Boolean)
    .map(term => term.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
    .join("|");
  if (!pattern) return str;

  const re = new RegExp(`(${pattern})`, "gi");
  const parts = str.split(re);
  if (parts.length <= 1) return str;
  return parts
    .map((part, i) => (i % 2 === 1 ? el("mark", { class: "search-highlight" }, part) : part))
    .filter(part => part !== "");
}

export function el(tag, props = {}, children = []) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value == null || value === false) continue;
    if (key === "class") node.className = value;
    else if (key === "dataset") Object.assign(node.dataset, value);
    else if (key === "html") node.innerHTML = value;
    else if (key in node) node[key] = value;
    else node.setAttribute(key, value);
  }
  for (const child of [].concat(children)) {
    if (child == null || child === false) continue;
    node.append(child.nodeType ? child : document.createTextNode(String(child)));
  }
  return node;
}
