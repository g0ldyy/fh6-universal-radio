import { $$, el } from "../dom.js";
import { db } from "../format.js";
import { EQ_BAND_LABELS, SCHEMA, SOURCE_SECTIONS } from "../schema.js";
import { t, getLang, SUPPORTED } from "../i18n.js";

function buildField(section, spec, cfg) {
    const [key, label, type, a, b, c] = spec;
    const id = `f-${section}-${key}`;
    const cur = cfg?.[section]?.[key];
    const dataset = { section, key };

    if (type === "checkbox") {
        return el("div", { class: "field checkbox" }, [
            el("input", { type: "checkbox", id, checked: !!cur, dataset }),
            el("label", { for: id }, label),
        ]);
    }

    if (type === "source-select") {
        const available = SOURCE_SECTIONS().filter(([s]) => cfg?.[s]?.enabled);
        const options = [el("option", { value: "" }, t("schema.select.none"))];
        for (const [value, name] of available) { options.push(el("option", { value, selected: cur === value }, name)); }
        if (cur && !available.some(([v]) => v === cur)) { options.push(el("option", { value: cur, selected: true }, cur)); }
        return el("div", { class: "field" }, [
            el("label", { for: id }, label),
            el("select", { id, dataset }, options),
        ]);
    }

    if (type === "station-select") {
        const list = cfg?.online_radio?.stations || [];
        const options = list.length
            ? list.map((s, i) => el("option", { value: String(i), selected: Number(cur) === i }, s.name || `Station ${i + 1}`))
            : [el("option", { value: "0" }, t("schema.select.no_stations"))];
        return el("div", { class: "field" }, [
            el("label", { for: id }, label),
            el("select", { id, dataset: { ...dataset, numeric: "1" } }, options),
        ]);
    }

    if (type === "select") {
        const options = (a || []).map(value => el("option", { value, selected: cur === value }, value));
        return el("div", { class: "field" }, [
            el("label", { for: id }, label),
            el("select", { id, dataset }, options),
        ]);
    }

  if (type === "select-kv") {
    const options = (a || []).map(([val, lbl]) => el("option", { value: String(val), selected: Number(cur) === val }, lbl));
    return el("div", { class: "field" }, [
      el("label", { for: id }, label),
      el("select", { id, dataset: { ...dataset, numeric: "1" } }, options),
    ]);
  }

  if (type === "bands") {
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
  }

    const input = el("input", { id, type, value: cur ?? "", dataset });
    if (type === "number") {
        if (a != null) input.min = String(a);
        if (b != null) input.max = String(b);
        input.step = String(c ?? 1);
    }
    return el("div", { class: "field" }, [el("label", { for: id }, label), input]);
}

export function renderSettings(form, cfg) {
    // Dynamic Color Feature: Retrieve user preference (default to true if not explicitly disabled)
    const dynamicColor = localStorage.getItem("fh6-dynamic-color") !== "false";
    const checkbox = el("input", { type: "checkbox", id: "f-dynamic-color" });

    // Language Selection: Map the central SUPPORTED array into dropdown option elements
    const browserLang = navigator.language?.slice(0, 2) || "en";
    const langSelect = el(
        "select", 
        { id: "f-language" }, 
        SUPPORTED.map(lang => el("option", { value: lang.code }, lang.label))
    );

    // Hydrate Inputs: Set initial values from active configurations
    checkbox.checked = dynamicColor;
    langSelect.value = getLang();

    // UI Layout: Construct the Interface fieldset with localized text
    const interfaceFieldset = el("fieldset", {}, [
        el("legend", {}, t("settings.interface.title")),
        
        // Dynamic Color Toggle
        el("div", { class: "field checkbox" }, [
            checkbox,
            el("label", { for: "f-dynamic-color" }, t("settings.interface.dynamic_color")),
        ]),
        
        // Language Picker Dropdown
        el("div", { class: "field" }, [
            el("label", { for: "f-language" }, t("settings.interface.language")),
            langSelect,
            el("span", { class: "field-hint" }, t("settings.interface.language_hint")),
        ]),
    ]);

    form.replaceChildren(
        interfaceFieldset,
        ...SCHEMA().map(([section, title, fields]) => {
            const fieldset = el("fieldset", {}, [el("legend", {}, title)]);
            for (const spec of fields) fieldset.append(buildField(section, spec, cfg));
            return fieldset;
        }),
    );
}

export function collectSettings(form) {
    const patch = {};
    for (const node of $$("[data-section]", form)) {
        const { section, key, index } = node.dataset;
        patch[section] ??= {};
        if (index !== undefined) {
            const band = (patch[section][key] ??= []);
            band[parseInt(index, 10)] = parseFloat(node.value);
        } else if (node.type === "checkbox") {
            patch[section][key] = node.checked;
        } else if (node.type === "number" || node.type === "range") {
            patch[section][key] = parseFloat(node.value);
        } else if (node.dataset.numeric) {
            patch[section][key] = parseInt(node.value, 10) || 0;
        } else {
            patch[section][key] = node.value;
        }
    }
    return patch;
}
