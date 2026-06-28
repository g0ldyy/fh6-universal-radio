// Render memoization. changed(key, signature) is true only when the signature
// differs from the last call for that key, so renders can skip no-op DOM work.
const signatures = new Map();

export function changed(key, signature) {
  if (signatures.get(key) === signature) return false;
  signatures.set(key, signature);
  return true;
}

export function resetMemo() {
  signatures.clear();
}