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
        const options = (a || []).map(value => el("option", { value, selected: cur === value }, t(`schema.playback.race_start.${value}`)));
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

  if (type === "keybind-kb" || type === "keybind-pad") {
    const isPad = type === "keybind-pad";
    const optionsData = a || [];

    const curNum = Number(cur || 0);
    const isPredefined = optionsData.some(([val]) => val === curNum);
    const isCustom = !isPredefined;

    const select = el("select", {}, optionsData.map(([val, lbl]) => {
      return el("option", { value: String(val), selected: val === curNum }, lbl);
    }));
    select.append(el("option", { value: "custom", selected: isCustom }, "Custom..."));
    
    // --- naming helpers ---
    const formatKb = (code) => {
      if (!code) return "";
      const vk = code & 0xFF; // base key
      const shift = (code & 0x0100) ? "Shift + " : "";
      const ctrl = (code & 0x0200) ? "Ctrl + " : "";
      const alt = (code & 0x0400) ? "Alt + " : "";
      
      let baseKey = vk;
      // convert standard letters and numbers to characters
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
        0x0100, 0x0200, 0x4000, 0x8000, 0x1000, 0x2000, // bumpers, face buttons
        0x0040, 0x0080, 0x0020, 0x0010, 0x0001, 0x0002, 0x0004, 0x0008 // sticks, d-pad
      ];
      
      for (const k of orderedKeys) {
        if ((code & k) === k) pressedNames.push(padMap[k]);
      }
      
      return pressedNames.length > 0 ? `${hex} (${pressedNames.join(" + ")})` : hex;
    };

    const initialCustomStr = isPad ? formatPad(curNum) : formatKb(curNum);

    // --- create custom input ---
    const customInput = el("input", {
      type: "text",
      placeholder: isPad ? "Click and press a controller button..." : "Click and press a key...",
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

    // --- dropdown logic ---
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

    // --- input capturing ---
    if (!isPad) {
      customInput.addEventListener("keydown", (e) => {
        e.preventDefault();
        
        if (e.keyCode === 27) { // escape clears the bind
          customInput.value = "";
          hiddenInput.value = "0";
          return;
        }
        
        // ignore solitary modifier presses
        if (e.keyCode === 16 || e.keyCode === 17 || e.keyCode === 18) return; 

        let mask = e.keyCode;
        
        // pack modifier states into the higher bits
        if (e.shiftKey) mask |= 0x0100;
        if (e.ctrlKey)  mask |= 0x0200;
        if (e.altKey)   mask |= 0x0400;

        customInput.value = formatKb(mask);
        if (select.value === "custom") hiddenInput.value = mask;
      });
    } else {
      // gamepad API polling for controllers
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
                // bitwise OR: adds the buttons hex value to running total
                currentMask |= gpMap[i]; 
              }
            }

            if (currentMask > 0) {
              const str = formatPad(currentMask);
              customInput.value = str;
              if (select.value === "custom") hiddenInput.value = str; 
            }
          }
        }, 50); // polls 20 times a second
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
    const { section, key, index, isHex, isNumeric } = node.dataset;
    patch[section] ??= {};
    if (isHex || isNumeric) {
      patch[section][key] = parseInt(node.value) || 0;
    } else if (index !== undefined) {
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
