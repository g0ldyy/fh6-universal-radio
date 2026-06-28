import { $$, el } from "../lib/dom.js";
import { db } from "../lib/format.js";
import { icons } from "../icons.js";
import { EQ_BAND_LABELS, SCHEMA, SOURCE_SECTIONS } from "../schema.js";
import { t, getLang, SUPPORTED } from "../i18n.js";
import { prefs } from "../preferences.js";

// The "interface" section is UI-only (stored in localStorage via
// preferences.js), never sent to the backend, so its current values are
// resolved here instead of from cfg.
function interfaceValue(key) {
    switch (key) {
        case "dynamic_color": return prefs.dynamicColor.get();
        case "perf_mode": return prefs.perfMode.get();
        case "language": return getLang();
        case "view_mode": return prefs.viewMode.get();
        case "theme_mode": return prefs.themeMode.get();
        default: return undefined;
    }
}

function buildKeybindField({ id, label, type, cur, dataset, args: [optionsArg] }) {
    const isPad = type === "keybind-pad";
    const optionsData = typeof optionsArg === "function" ? optionsArg() : (optionsArg || []);

    const curNum = Number(cur || 0);
    const isPredefined = optionsData.some(([val]) => val === curNum);
    const isCustom = !isPredefined;

    const select = el("select", {}, optionsData.map(([val, lbl]) => {
        return el("option", { value: String(val), selected: val === curNum }, lbl);
    }));
    select.append(el("option", { value: "custom", selected: isCustom }, t("hotkeys.custom")));

    // --- naming helpers ---
    const formatKb = (code) => {
        if (!code) return "";
        const vk = code & 0xFF; // base key
        const shift = (code & 0x0100) ? "Shift + " : "";
        const ctrl = (code & 0x0200) ? "Ctrl + " : "";
        const alt = (code & 0x0400) ? "Alt + " : "";

        let baseKey = vk;
        if ((vk >= 65 && vk <= 90) || (vk >= 48 && vk <= 57)) {
            baseKey = String.fromCharCode(vk);
        }
        return `${ctrl}${shift}${alt}${baseKey}`;
    };

    const padMap = {
        0x1000: "A", 0x2000: "B", 0x4000: "X", 0x8000: "Y",
        0x0100: "LB", 0x0200: "RB", 0x0020: "View", 0x0010: "Menu",
        0x0040: "LS", 0x0080: "RS", 0x0001: "D-Up", 0x0002: "D-Down",
        0x0004: "D-Left", 0x0008: "D-Right"
    };

    const formatPad = (code) => {
        if (!code) return "";
        const hex = "0x" + code.toString(16).toUpperCase();

        const pressedNames = [];
        const orderedKeys = [
            0x0100, 0x0200, 0x4000, 0x8000, 0x1000, 0x2000,
            0x0040, 0x0080, 0x0020, 0x0010, 0x0001, 0x0002, 0x0004, 0x0008
        ];

        for (const k of orderedKeys) {
            if ((code & k) === k) pressedNames.push(padMap[k]);
        }

        return pressedNames.length > 0 ? `${hex} (${pressedNames.join(" + ")})` : hex;
    };

    const initialCustomStr = isPad ? formatPad(curNum) : formatKb(curNum);

    const customInput = el("input", {
        type: "text",
        placeholder: isPad ? t("hotkeys.press_pad") : t("hotkeys.press_kb"),
        readOnly: true,
        value: isCustom ? initialCustomStr : ""
    });

    customInput.style.marginTop = "0.5rem";
    customInput.style.display = isCustom ? "block" : "none";

    const customDataset = isPad ? { isHex: "1" } : { isNumeric: "1" };
    const hiddenInput = el("input", {
        type: "hidden",
        id,
        dataset: { ...dataset, ...customDataset },
        value: isCustom ? initialCustomStr : curNum
    });

    select.addEventListener("change", () => {
        if (select.value === "custom") {
            customInput.style.display = "block";
            customInput.focus();
            hiddenInput.value = customInput.value || 0;
        } else {
            customInput.style.display = "none";
            hiddenInput.value = select.value;
        }
    });

    if (!isPad) {
        customInput.addEventListener("keydown", (e) => {
            e.preventDefault();
            if (e.keyCode === 27) {
                customInput.value = "";
                hiddenInput.value = "0";
                return;
            }
            if (e.keyCode === 16 || e.keyCode === 17 || e.keyCode === 18) return;

            let mask = e.keyCode;
            if (e.shiftKey) mask |= 0x0100;
            if (e.ctrlKey) mask |= 0x0200;
            if (e.altKey) mask |= 0x0400;

            customInput.value = formatKb(mask);
            if (select.value === "custom") hiddenInput.value = mask;
        });
    } else {
        let padInterval;
        const gpMap = [
            0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, null, null,
            0x0020, 0x0010, 0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008
        ];

        customInput.addEventListener("focus", () => {
            padInterval = setInterval(() => {
                const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
                for (let gp of gamepads) {
                    if (!gp) continue;
                    let currentMask = 0;
                    for (let i = 0; i < gp.buttons.length; i++) {
                        if (gp.buttons[i].pressed && gpMap[i]) {
                            currentMask |= gpMap[i];
                        }
                    }
                    if (currentMask > 0) {
                        const str = formatPad(currentMask);
                        customInput.value = str;
                        if (select.value === "custom") hiddenInput.value = str;
                    }
                }
            }, 50);
        });

        customInput.addEventListener("blur", () => clearInterval(padInterval));
    }

    return el("div", { class: "field" }, [
        el("label", { for: id }, label),
        select,
        customInput,
        hiddenInput
    ]);
}

// One renderer per schema field "type" — keeps buildField() a thin dispatch
// table instead of a long if/else chain (same factory pattern as sourceApi
// in api.js and createStationManager).
const fieldRenderers = {
    checkbox: ({ id, label, cur, dataset }) =>
        el("div", { class: "field checkbox" }, [
            el("input", { type: "checkbox", id, checked: !!cur, dataset }),
            el("label", { for: id }, label),
        ]),

    "language-select": ({ id, label, cur, dataset, args: [hint] }) => {
        const options = SUPPORTED.map(lang =>
            el("option", { value: lang.code, selected: cur === lang.code }, lang.label)
        );
        return el("div", { class: "field" }, [
            el("label", { for: id }, label),
            el("select", { id, dataset }, options),
            hint ? el("span", { class: "field-hint" }, hint) : null,
        ].filter(Boolean));
    },

    "view-select": ({ id, label, cur, dataset }) => {
        const options = [
            el("option", { value: "default", selected: cur === "default" }, t("settings.interface.view.default")),
            el("option", { value: "minimal", selected: cur === "minimal" }, t("settings.interface.view.minimal")),
        ];
        return el("div", { class: "field" }, [el("label", { for: id }, label), el("select", { id, dataset }, options)]);
    },

    "theme-select": ({ id, label, cur, dataset }) => {
        const options = [
            el("option", { value: "dark", selected: cur === "dark" }, t("settings.interface.theme.dark")),
            el("option", { value: "light", selected: cur === "light" }, t("settings.interface.theme.light")),
            el("option", { value: "dark-squared", selected: cur === "dark-squared" }, t("settings.interface.theme.dark_squared")),
            el("option", { value: "light-squared", selected: cur === "light-squared" }, t("settings.interface.theme.light_squared")),
        ];
        return el("div", { class: "field" }, [el("label", { for: id }, label), el("select", { id, dataset }, options)]);
    },

    "source-select": ({ id, label, cur, dataset, cfg }) => {
        const available = SOURCE_SECTIONS().filter(([s]) => cfg?.[s]?.enabled);
        const options = [el("option", { value: "" }, t("schema.select.none"))];
        for (const [value, name] of available) options.push(el("option", { value, selected: cur === value }, name));
        if (cur && !available.some(([v]) => v === cur)) options.push(el("option", { value: cur, selected: true }, cur));
        return el("div", { class: "field" }, [el("label", { for: id }, label), el("select", { id, dataset }, options)]);
    },

    "station-select": ({ id, label, cur, dataset, cfg }) => {
        const list = cfg?.online_radio?.stations || [];
        const options = list.length
            ? list.map((s, i) => el("option", { value: String(i), selected: Number(cur) === i }, s.name || `Station ${i + 1}`))
            : [el("option", { value: "0" }, t("schema.select.no_stations"))];
        return el("div", { class: "field" }, [
            el("label", { for: id }, label),
            el("select", { id, dataset: { ...dataset, numeric: "1" } }, options),
        ]);
    },

    select: ({ id, label, cur, dataset, args: [values] }) => {
        const options = (values || []).map(value => el("option", { value, selected: cur === value }, t(`schema.playback.race_start.${value}`)));
        return el("div", { class: "field" }, [el("label", { for: id }, label), el("select", { id, dataset }, options)]);
    },

    "select-kv": ({ id, label, cur, dataset, args: [pairs] }) => {
        const options = (pairs || []).map(([val, lbl]) => el("option", { value: String(val), selected: Number(cur) === val }, lbl));
        return el("div", { class: "field" }, [
            el("label", { for: id }, label),
            el("select", { id, dataset: { ...dataset, numeric: "1" } }, options),
        ]);
    },

    bands: ({ label, cur, section, key }) => {
        const values = Array.isArray(cur) ? cur : [0, 0, 0, 0, 0];
        const rows = EQ_BAND_LABELS.map((bandLabel, i) => {
            const value = values[i] ?? 0;
            const out = el("output", {}, db(value));
            const range = el("input", {
                type: "range",
                min: "-6",
                max: "6",
                step: "0.5",
                value: String(value),
                "aria-label": bandLabel,
                dataset: { section, key, index: String(i) },
            });
            range.addEventListener("input", () => {
                out.textContent = db(parseFloat(range.value));
            });
            return el("div", { class: "band" }, [el("span", { class: "band-label" }, bandLabel), range, out]);
        });
        return el("div", { class: "field bands" }, [el("label", {}, label), ...rows]);
    },

    "keybind-kb": ctx => buildKeybindField(ctx),
    "keybind-pad": ctx => buildKeybindField(ctx),
};

function defaultField({ id, label, type, cur, dataset, args: [min, max, step] }) {
    const input = el("input", { id, type, value: cur ?? "", dataset });
    let errorSpan = null;

    if (type === "number") {
        if (min != null) input.min = String(min);
        if (max != null) input.max = String(max);
        input.step = String(step ?? 1);

        errorSpan = el("span", { class: "field-error" });
        const validate = () => {
            const val = parseFloat(input.value);
            const outOfRange = input.value !== "" && ((min != null && val < min) || (max != null && val > max));
            input.classList.toggle("invalid", outOfRange);
            errorSpan.textContent = outOfRange ? t("settings.value_out_of_range", { min, max }) : "";
        };
        input.addEventListener("input", validate);
        validate();
    }

    return el("div", { class: "field" }, [el("label", { for: id }, label), input, errorSpan].filter(Boolean));
}

// Types whose visible state lives entirely in the [data-section][data-key]
// node(s) themselves (value/checked), so resetting can just restore that
// value and dispatch input/change — covers everything except the keybind
// types, whose select+custom-input UI is independently maintained and would
// need a full rebuild to reset correctly (not worth it for a 10-field edge case).
function isResettable(type) {
    return !type.startsWith("keybind");
}

function buildField(section, spec, cfg) {
    const [key, label, type, a, b, c] = spec;
    const id = `f-${section}-${key}`;
    const cur = section === "interface" ? interfaceValue(key) : cfg?.[section]?.[key];
    const ctx = { id, label, type, cur, cfg, section, key, dataset: { section, key }, args: [a, b, c] };
    const field = (fieldRenderers[type] || defaultField)(ctx);

    if (isResettable(type)) {
        field.dataset.resetSection = section;
        field.dataset.resetKey = key;
        field.append(el("button", {
            type: "button",
            class: "field-reset-btn",
            "aria-label": t("settings.reset_field"),
            title: t("settings.reset_field"),
            html: icons.undo,
        }));
    }

    return field;
}

// Restores a field's node(s) to their value in `baseline` (the patch
// captured when the form was last rendered from a saved state) and fires
// input/change so any dependent UI (band dB readout, section-disabled
// toggle, the modified-indicator) reacts exactly as if the user had typed it.
export function resetFieldToBaseline(field, section, key, baseline) {
    const saved = baseline?.[section]?.[key];
    const nodes = $$("[data-section]", field).filter(n => n.dataset.section === section && n.dataset.key === key);

    for (const node of nodes) {
        const value = node.dataset.index !== undefined ? saved?.[parseInt(node.dataset.index, 10)] : saved;
        if (node.type === "checkbox") node.checked = !!value;
        else node.value = value ?? "";
        node.dispatchEvent(new Event("input", { bubbles: true }));
        node.dispatchEvent(new Event("change", { bubbles: true }));
    }
}

// Flags fields whose current value differs from `baseline`, so the user can
// see at a glance what they're about to change (and where the reset button
// in resetFieldToBaseline applies). Call on every form input/change.
export function markModifiedFields(form, baseline) {
    const modified = new Map();
    for (const node of $$("[data-section]", form)) {
        const { section, key, index } = node.dataset;
        if (!section || !key) continue;
        const field = node.closest(".field");
        if (!field) continue;

        const saved = index !== undefined ? baseline?.[section]?.[key]?.[parseInt(index, 10)] : baseline?.[section]?.[key];
        const changed = saved !== parseFieldValue(node);
        modified.set(field, (modified.get(field) || false) || changed);
    }
    for (const [field, changed] of modified) field.classList.toggle("field-modified", changed);
}

export function renderSettings(form, cfg) {
    form.replaceChildren(
        ...SCHEMA().map(([section, title, fields]) => {
            const fieldset = el("fieldset", {}, [el("legend", {}, title)]);
            for (const spec of fields) {
                fieldset.append(buildField(section, spec, cfg));
            }

            const enabledCheckbox = fieldset.querySelector('input[data-key="enabled"]');
            if (enabledCheckbox) {
                const toggleFields = () => {
                    fieldset.classList.toggle("section-disabled", !enabledCheckbox.checked);
                };
                enabledCheckbox.addEventListener("change", toggleFields);
                toggleFields();
            }
            return fieldset;
        }),
    );
}

// Parsers tried in order; the first whose predicate matches the node wins.
// Bands are handled separately below since they accumulate into an array.
const VALUE_PARSERS = [
    [node => node.dataset.isHex || node.dataset.isNumeric, node => parseInt(node.value, 10) || 0],
    [node => node.type === "checkbox", node => node.checked],
    [node => node.type === "number" || node.type === "range", node => parseFloat(node.value)],
    [node => node.dataset.numeric, node => parseInt(node.value, 10) || 0],
];

function parseFieldValue(node) {
    const parser = VALUE_PARSERS.find(([test]) => test(node));
    return parser ? parser[1](node) : node.value;
}

// Pure read of the form's current values — no side effects, safe to call
// repeatedly (e.g. to detect unsaved changes before the user commits them).
export function collectSettings(form) {
    const patch = {};
    for (const node of $$("[data-section]", form)) {
        const { section, key, index } = node.dataset;
        if (!section || !key) continue;
        patch[section] ??= {};

        if (index !== undefined) {
            const band = (patch[section][key] ??= []);
            band[parseInt(index, 10)] = parseFloat(node.value);
            continue;
        }

        patch[section][key] = parseFieldValue(node);
    }
    return patch;
}

// Persists the "interface" section to localStorage and strips it from the
// patch, since it's UI-only and must never be sent to the backend. Call this
// only when the user actually commits the form (Save), not when merely
// reading it for comparison.
export function applyInterfacePrefs(patch) {
    const iface = patch.interface;
    if (!iface) return patch;
    if (iface.dynamic_color !== undefined) prefs.dynamicColor.set(iface.dynamic_color);
    if (iface.perf_mode !== undefined) prefs.perfMode.set(iface.perf_mode);
    if (iface.language !== undefined) prefs.language.set(iface.language);
    if (iface.view_mode !== undefined) prefs.viewMode.set(iface.view_mode);
    if (iface.theme_mode !== undefined) prefs.themeMode.set(iface.theme_mode);
    delete patch.interface;
    return patch;
}
