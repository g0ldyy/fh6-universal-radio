// Triggers a browser "Save as" for an in-memory JSON object — used by the
// settings backup and station pack export buttons.
export function downloadJson(data, filename) {
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
}

export function todayStamp() {
    return new Date().toISOString().split("T")[0];
}
