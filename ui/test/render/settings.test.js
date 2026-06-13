import { describe, it, expect, beforeEach } from "vitest";
import { renderSettings, collectSettings } from "../../dist/js/render/settings.js";

beforeEach(() => {
  document.body.innerHTML = '<form id="f"></form>';
});

const form = () => document.getElementById("f");

describe("renderSettings", () => {
  it("builds every section and the right field types", () => {
    renderSettings(form(), {
      general: { port: 8420 },
      local_files: { enabled: true },
      playback: { equalizer_bands: [1, 2, 3, 4, 5], race_start_playback: "restart" },
      tidal: { enabled: true, client_id: "my-id", audio_quality: "LOSSLESS" },
    });
    const tidalEnabled = form().querySelector("#f-tidal-enabled");
    expect(tidalEnabled).not.toBeNull();
    expect(tidalEnabled.checked).toBe(true);
    expect(form().querySelector("#f-tidal-client_id").value).toBe("my-id");
    expect(form().querySelector("#f-tidal-audio_quality").value).toBe("LOSSLESS");
    expect(form().querySelector("#f-general-port").value).toBe("8420");
    expect(form().querySelector("#f-local_files-enabled").checked).toBe(true);

    const select = form().querySelector("#f-playback-race_start_playback");
    expect(select.tagName).toBe("SELECT");
    expect(select.value).toBe("restart");

    const bands = form().querySelectorAll('.field.bands input[type="range"]');
    expect(bands.length).toBe(5);
    expect(bands[2].value).toBe("3");
    expect([...bands].map(b => b.getAttribute("aria-label"))).toEqual([
      "60 Hz",
      "250 Hz",
      "1 kHz",
      "4 kHz",
      "12 kHz",
    ]);
  });

  it("EQ slider updates its dB readout on input", () => {
    renderSettings(form(), { playback: { equalizer_bands: [0, 0, 0, 0, 0] } });
    const range = form().querySelector('.field.bands input[type="range"]');
    range.value = "2.5";
    range.dispatchEvent(new Event("input"));
    expect(range.nextElementSibling.textContent).toBe("2.5 dB");
  });
});

describe("collectSettings", () => {
  it("collects typed values and the full EQ band array", () => {
    renderSettings(form(), {
      general: { port: 8420 },
      audio: { output_gain: 0.5 },
      local_files: { enabled: false },
      playback: { equalizer_bands: [0, 0, 0, 0, 0] },
    });
    form().querySelector("#f-local_files-enabled").checked = true;
    form().querySelector("#f-general-port").value = "9000";
    form().querySelectorAll('.field.bands input[type="range"]')[0].value = "3";

    const patch = collectSettings(form());
    expect(patch.local_files.enabled).toBe(true);
    expect(patch.general.port).toBe(9000);
    expect(patch.audio.output_gain).toBe(0.5);
    expect(patch.playback.equalizer_bands).toEqual([3, 0, 0, 0, 0]);
  });
});
