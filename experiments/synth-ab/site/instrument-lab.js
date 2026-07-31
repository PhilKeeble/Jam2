(() => {
  const manifest = window.JAM2_SOUND_DESIGN_MANIFEST;
  if (!manifest) return;

  const storageKey = "jam2-instrument-lab-v1";
  const pitchedRoles = new Set(["chords", "melody", "bass", "support"]);
  const labRoles = new Set([...pitchedRoles, "drums"]);
  const sourceOptions = [
    ["jam2-native", "Jam2 native voice"],
    ["variable-shape", "Variable-shape oscillator"],
    ["variable-saw", "Variable-saw oscillator"],
    ["fm2", "Two-operator FM"],
    ["string-physical-model", "Physical string"],
    ["additive-harmonic", "Additive harmonic"],
    ["sine-fundamental", "Sine fundamental"],
    ["phase-reset-formant", "Phase-reset formant"],
    ["vosim-formant", "VOSIM formant"],
    ["z-oscillator", "Z oscillator"],
  ];
  const drumPieces = [
    ["kick", "Kick"], ["snare", "Snare"], ["closed-hat", "Closed hat"],
    ["open-hat", "Open hat"], ["high-tom", "High tom"],
    ["mid-tom", "Mid tom"], ["floor-tom", "Floor tom"],
    ["crash", "Crash"], ["ride", "Ride"], ["cross-stick", "Cross-stick"],
    ["shaker", "Shaker"], ["hand-percussion", "Hand percussion"],
  ];
  const drumSourceOptions = [
    ["jam2-native", "Jam2 native"],
    ["daisy-profile", "Daisy researched profile model"],
    ["daisy-analog-kick", "Daisy analog kick"],
    ["daisy-synthetic-kick", "Daisy synthetic kick"],
    ["daisy-analog-snare", "Daisy analog snare"],
    ["daisy-synthetic-snare", "Daisy synthetic snare"],
    ["jam2-shell-snare", "Jam2 shell-and-wire snare"],
    ["daisy-metal", "Daisy metallic hat / cymbal"],
    ["daisy-ring-metal", "Daisy ring-mod metallic source"],
    ["daisy-modal", "Daisy modal resonator"],
    ["daisy-particle", "Daisy particle resonator"],
    ["daisy-cymbal", "Daisy noise + metal cymbal"],
    ["jam2-shell-tom", "Jam2 damped shell tom"],
    ["jam2-cross-stick", "Jam2 wood/rim cross-stick"],
    ["jam2-shaker", "Jam2 collision shaker"],
    ["jam2-hand-drum", "Jam2 skin hand drum"],
    ["jam2-hand-clap", "Jam2 multi-burst hand clap"],
    ["jam2-wood-block", "Jam2 wood block"],
    ["jam2-tambourine", "Jam2 tambourine"],
    ["jam2-crash-cymbal", "Jam2 diffuse crash cymbal"],
    ["jam2-ride-cymbal", "Jam2 stick-defined ride cymbal"],
  ];
  const drumTransientOptions = [
    ["off", "Off"],
    ["soft-beater", "Soft beater"],
    ["hard-beater", "Hard beater"],
    ["stick", "Stick"],
    ["head-strike", "Drum-head strike"],
    ["rim", "Rim / shell"],
    ["click", "Click / chirp"],
    ["brush", "Brush / tap"],
    ["clap", "Multi-burst clap"],
  ];
  const drumTextureOptions = [
    ["off", "Off"],
    ["wire", "Snare wire"],
    ["dust", "Dust / sparse grit"],
    ["particle", "Particle / rattle"],
    ["air", "Filtered air"],
    ["metal-wash", "Metallic wash"],
  ];
  const drumSynthSourceOptions = [
    ["off", "Off"],
    ...sourceOptions.filter(([value]) => value !== "jam2-native"),
  ];
  const mixRoleGains = {
    default: .82,
    drums: 1.16,
    support: .68,
  };
  const drumParameters = [
    {key: "frequencyHz", label: "Pitch / centre frequency", min: 20, max: 12000, step: 1, unit: " Hz"},
    {key: "tone", label: "Tone", min: 0, max: 1, step: .01},
    {key: "decay", label: "Decay", min: .001, max: 1, step: .001},
    {key: "colour", label: "Noise / snap / metal colour", min: 0, max: 1, step: .01},
    {key: "fmAmount", label: "Pitch / FM sweep", min: 0, max: 1, step: .01},
    {key: "level", label: "Piece level", min: 0, max: 1.5, step: .01},
  ];
  const drumSynthParameters = [
    {key: "midiNote", label: "Pitch (MIDI note)", min: 24, max: 96, step: 1},
    {key: "level", label: "Synth layer level", min: 0, max: 1.5, step: .01},
    {key: "gateSeconds", label: "Gate", min: .005, max: 2, step: .005, unit: " s"},
    {key: "attackSeconds", label: "Attack", min: .001, max: .25, step: .001, unit: " s"},
    {key: "decaySeconds", label: "Decay", min: .005, max: 2, step: .005, unit: " s"},
    {key: "sustain", label: "Sustain", min: .01, max: 1, step: .01},
    {key: "releaseSeconds", label: "Release", min: .005, max: 3, step: .005, unit: " s"},
    {key: "noiseMix", label: "Noise", min: 0, max: 1, step: .01},
    {key: "filterCutoffHz", label: "Low-pass cutoff", min: 40, max: 18000, step: 10, unit: " Hz"},
  ];
  const drumComponentParameters = [
    {object: "transient", key: "level", label: "Transient level", min: 0, max: 1.5, step: .01},
    {object: "transient", key: "tone", label: "Transient colour", min: 0, max: 1, step: .01},
    {object: "transient", key: "decaySeconds", label: "Transient decay", min: .001, max: .25, step: .001, unit: " s"},
    {object: "texture", key: "level", label: "Texture level", min: 0, max: 1.5, step: .01},
    {object: "texture", key: "tone", label: "Texture colour", min: 0, max: 1, step: .01},
    {object: "texture", key: "decaySeconds", label: "Texture decay", min: .003, max: 4, step: .003, unit: " s"},
    {object: "texture", key: "density", label: "Texture density", min: 0, max: 1, step: .01},
    {object: "colourStage", key: "voiceDrive", label: "Per-voice drive", min: .5, max: 12, step: .01},
    {object: "colourStage", key: "sampleRateHz", label: "Internal source rate", min: 1000, max: 48000, step: 10, unit: " Hz"},
    {object: "colourStage", key: "bitDepth", label: "Bit depth", min: 4, max: 24, step: 1, integer: true},
    {object: "colourStage", key: "reconstructionLowpassHz", label: "Reconstruction low-pass", min: 200, max: 20000, step: 10, unit: " Hz"},
    {object: "colourStage", key: "dynamicFilterAmount", label: "Dynamic reconstruction", min: 0, max: 1, step: .01},
    {key: "roomSend", label: "Shared room send", min: 0, max: 1, step: .01},
    {object: "relationship", key: "chokeSeconds", label: "Choke fade", min: .001, max: .25, step: .001, unit: " s"},
  ];
  const drumVelocityParameters = [
    {band: "ghost", key: "minimum", label: "Ghost minimum", min: 1, max: 127, step: 1, integer: true},
    {band: "ghost", key: "maximum", label: "Ghost maximum", min: 1, max: 127, step: 1, integer: true},
    {band: "normal", key: "minimum", label: "Normal minimum", min: 1, max: 127, step: 1, integer: true},
    {band: "normal", key: "maximum", label: "Normal maximum", min: 1, max: 127, step: 1, integer: true},
    {band: "accent", key: "minimum", label: "Accent minimum", min: 1, max: 127, step: 1, integer: true},
    {band: "accent", key: "maximum", label: "Accent maximum", min: 1, max: 127, step: 1, integer: true},
    {key: "excitationCurve", label: "Excitation curve", min: .2, max: 3, step: .01},
    {key: "outputCurve", label: "Output curve", min: .2, max: 3, step: .01},
    {key: "brightnessAmount", label: "Velocity to brightness", min: 0, max: 1, step: .01},
    {key: "decayAmount", label: "Velocity to decay", min: -.8, max: .8, step: .01},
    {key: "driveAmount", label: "Velocity to drive", min: 0, max: 1.5, step: .01},
  ];
  const drumBusParameters = [
    {key: "drive", label: "Bus drive", min: .5, max: 8, step: .01},
    {key: "lowpassHz", label: "Bus low-pass", min: 200, max: 20000, step: 10, unit: " Hz"},
    {key: "compressorThreshold", label: "Compressor threshold", min: .01, max: 1, step: .005},
    {key: "compressorRatio", label: "Compressor ratio", min: 1, max: 20, step: .1},
    {key: "compressorReleaseMs", label: "Compressor release", min: 5, max: 500, step: 1, unit: " ms"},
    {key: "roomMix", label: "Shared room return", min: 0, max: .6, step: .005},
    {key: "roomSizeMs", label: "Room size", min: 5, max: 140, step: 1, unit: " ms"},
    {key: "roomDamping", label: "Room damping", min: 0, max: 1, step: .01},
  ];
  const filterOptions = [
    ["ladder-lowpass", "Ladder low-pass"],
    ["state-variable-lowpass", "State-variable low-pass"],
    ["state-variable-bandpass", "State-variable band-pass"],
    ["source-direct", "Direct source"],
  ];

  const parameters = [
    {group: "Oscillator", key: "shape", label: "Shape", min: 0, max: 1, step: .01, sources: ["variable-shape", "variable-saw"]},
    {group: "Oscillator", key: "width", label: "Pulse / notch width", min: .05, max: .95, step: .01, sources: ["variable-shape", "variable-saw"]},
    {group: "Oscillator", key: "oscillator2Mix", label: "Second oscillator", min: 0, max: 1, step: .01, sources: ["variable-shape", "variable-saw", "additive-harmonic"]},
    {group: "Oscillator", key: "detuneCents", label: "Detune", min: -50, max: 50, step: .5, unit: " ct", sources: ["variable-shape", "variable-saw", "string-physical-model"]},
    {group: "Oscillator", key: "subMix", label: "Sub fundamental", min: 0, max: 1, step: .01, sources: ["variable-shape", "variable-saw", "fm2"]},
    {group: "FM", key: "fmRatio", label: "FM ratio", min: .125, max: 16, step: .125, sources: ["fm2"]},
    {group: "FM", key: "fmIndex", label: "FM index", min: 0, max: 16, step: .05, sources: ["fm2"]},
    {group: "Harmonics", key: "harmonicFamily", label: "Harmonic family", min: 0, max: 5, step: 1, sources: ["additive-harmonic"]},
    {group: "Formants", key: "formantRatio", label: "Formant ratio 1", min: .25, max: 16, step: .05, sources: ["phase-reset-formant", "vosim-formant", "z-oscillator"]},
    {group: "Formants", key: "formantRatio2", label: "Formant ratio 2", min: .25, max: 16, step: .05, sources: ["vosim-formant"]},
    {group: "Formants", key: "fixedFormantHz", label: "Fixed formant 1", min: 0, max: 12000, step: 25, unit: " Hz", sources: ["phase-reset-formant", "vosim-formant"]},
    {group: "Formants", key: "fixedFormant2Hz", label: "Fixed formant 2", min: 0, max: 12000, step: 25, unit: " Hz", sources: ["vosim-formant"]},
    {group: "Formants", key: "spectralShape", label: "Spectral shape", min: 0, max: 1, step: .01, sources: ["phase-reset-formant", "vosim-formant", "z-oscillator"]},
    {group: "Formants", key: "spectralMode", label: "Spectral mode", min: 0, max: 1, step: .01, sources: ["z-oscillator"]},
    {group: "Physical string", key: "stringStructure", label: "Structure", min: 0, max: 1, step: .01, sources: ["string-physical-model"]},
    {group: "Physical string", key: "stringBrightness", label: "String brightness", min: 0, max: 1, step: .01, sources: ["string-physical-model"]},
    {group: "Physical string", key: "stringDamping", label: "Damping", min: 0, max: 1, step: .01, sources: ["string-physical-model"]},
    {group: "Physical string", key: "stringDouble", label: "Double-string layer", min: 0, max: 1, step: .01, sources: ["string-physical-model"]},
    {group: "Amplitude", key: "attackSeconds", label: "Attack", min: .001, max: 5, step: .001, unit: " s"},
    {group: "Amplitude", key: "decaySeconds", label: "Decay", min: .005, max: 5, step: .005, unit: " s"},
    {group: "Amplitude", key: "sustain", label: "Sustain", min: .01, max: 1, step: .01},
    {group: "Amplitude", key: "releaseSeconds", label: "Release", min: .005, max: 8, step: .005, unit: " s"},
    {group: "Filter", key: "filterCutoffHz", label: "Cutoff", min: 40, max: 18000, step: 10, unit: " Hz"},
    {group: "Filter", key: "filterEnvelopeHz", label: "Envelope depth", min: -12000, max: 12000, step: 10, unit: " Hz"},
    {group: "Filter", key: "resonance", label: "Resonance", min: 0, max: .95, step: .01},
    {group: "Filter", key: "filterDrive", label: "Filter drive", min: .5, max: 8, step: .01},
    {group: "Character", key: "wavefold", label: "Wavefold", min: 0, max: 8, step: .01},
    {group: "Character", key: "noiseMix", label: "Noise", min: 0, max: 1, step: .01},
    {group: "Character", key: "transientMix", label: "Transient layer", min: 0, max: 1, step: .005},
    {group: "Character", key: "transientSeconds", label: "Transient length", min: .001, max: .25, step: .001, unit: " s"},
    {group: "Character", key: "voiceDrive", label: "Voice drive", min: .5, max: 10, step: .01},
    {group: "Character", key: "busDrive", label: "Bus drive", min: .5, max: 10, step: .01},
    {group: "Character", key: "cabinet", label: "Cabinet colour", min: 0, max: 1, step: .01},
    {group: "Movement", key: "vibratoCents", label: "Vibrato depth", min: 0, max: 100, step: .25, unit: " ct"},
    {group: "Movement", key: "vibratoRateHz", label: "Vibrato rate", min: .05, max: 15, step: .05, unit: " Hz"},
    {group: "Movement", key: "vibratoDelaySeconds", label: "Vibrato delay", min: 0, max: 3, step: .01, unit: " s"},
    {group: "Movement", key: "tremoloDepth", label: "Tremolo depth", min: 0, max: 1, step: .01},
    {group: "Movement", key: "tremoloRateHz", label: "Tremolo rate", min: .02, max: 20, step: .02, unit: " Hz"},
    {group: "Space", key: "chorusMix", label: "Chorus mix", min: 0, max: 1, step: .01},
    {group: "Space", key: "chorusDepth", label: "Chorus depth", min: 0, max: 1, step: .01},
    {group: "Space", key: "chorusRateHz", label: "Chorus rate", min: .02, max: 8, step: .02, unit: " Hz"},
    {group: "Space", key: "delayMix", label: "Delay mix", min: 0, max: .75, step: .01},
    {group: "Space", key: "delaySeconds", label: "Delay time", min: .03, max: 1.5, step: .01, unit: " s"},
  ];
  const parameterMap = new Map(parameters.map(parameter => [parameter.key, parameter]));

  const macroDefinitions = [
    ["hardness", "Soft ↔ hard attack"],
    ["brightness", "Dark ↔ bright"],
    ["body", "Thin ↔ full body"],
    ["grit", "Clean ↔ gritty"],
    ["movement", "Stable ↔ animated"],
    ["length", "Short ↔ sustained"],
    ["space", "Dry ↔ spacious"],
  ];

  const lab = document.querySelector("#instrument-lab");
  const workbench = document.querySelector("#workbench");
  const profileSelect = document.querySelector("#lab-profile");
  const roleSelect = document.querySelector("#lab-role");
  const sourceSelect = document.querySelector("#lab-source");
  const sourceBSelect = document.querySelector("#lab-source-b");
  const filterSelect = document.querySelector("#lab-filter");
  const auditionSelect = document.querySelector("#lab-audition");
  const rootSelect = document.querySelector("#lab-root");
  const renderButton = document.querySelector("#lab-render");
  const replayButton = document.querySelector("#lab-play");
  const mixButton = document.querySelector("#lab-mix");
  const jam2BackingButton = document.querySelector("#lab-mix-jam2");
  const jam2DrumsButton = document.querySelector("#lab-jam2-drums");
  const jam2ReferenceButton =
    document.querySelector("#lab-mix-jam2-reference");
  const renderStatus = document.querySelector("#lab-render-status");

  let saved = loadSaved();
  let profile = null;
  let role = null;
  let patch = null;
  let researchedPatch = null;
  let macroBase = null;
  let macroValues = Object.fromEntries(macroDefinitions.map(([id]) => [id, 0]));
  let currentUrl = "";
  let soloAudio = null;
  let audioContext = null;
  let mixSources = [];
  let renderAbort = null;
  let autoRenderTimer = 0;
  let drumPiece = "kick";
  let drumKitCandidate = "";
  let lastRenderDiagnostics = null;

  populateStaticControls();
  bindEvents();
  selectInitialProfile();
  checkService();

  function populateStaticControls() {
    for (const item of manifest.profiles) {
      const option = document.createElement("option");
      option.value = item.id;
      option.textContent = `${styleName(item.styleId)} — ${item.name}`;
      profileSelect.append(option);
    }
    for (let midi = 28; midi <= 84; ++midi) {
      const option = document.createElement("option");
      option.value = String(midi);
      option.textContent = `${noteName(midi)} (${midi})`;
      rootSelect.append(option);
    }
    for (const [value, label] of sourceOptions) {
      sourceSelect.add(new Option(label, value));
      sourceBSelect.add(new Option(label, value));
    }
    sourceBSelect.add(new Option("Off", "off"), 0);
    const drumSource = document.querySelector("#lab-drum-source");
    const drumSourceB = document.querySelector("#lab-drum-source-b");
    for (const [value, label] of drumSourceOptions) {
      drumSource.add(new Option(label, value));
      drumSourceB.add(new Option(label, value));
    }
    drumSourceB.add(new Option("Off", "off"), 0);
    const drumSynthSource =
      document.querySelector("#lab-drum-synth-source");
    drumSynthSource.replaceChildren();
    for (const [value, label] of drumSynthSourceOptions) {
      drumSynthSource.add(new Option(label, value));
    }
    const transientSelect =
      document.querySelector("#lab-drum-transient");
    for (const [value, label] of drumTransientOptions) {
      transientSelect.add(new Option(label, value));
    }
    const textureSelect =
      document.querySelector("#lab-drum-texture");
    for (const [value, label] of drumTextureOptions) {
      textureSelect.add(new Option(label, value));
    }
    for (const [value, label] of filterOptions) {
      filterSelect.add(new Option(label, value));
    }
    const macroRoot = document.querySelector("#lab-macros");
    for (const [id, label] of macroDefinitions) {
      const control = document.createElement("label");
      control.className = "macro-control";
      control.innerHTML = `
        <span><strong>${label}</strong><output>Centre</output></span>
        <input type="range" min="-100" max="100" value="0" step="1" data-macro="${id}">
      `;
      macroRoot.append(control);
    }
    const reactions = [
      "Too bright", "Too dull", "Too thin", "Too heavy",
      "Attack too sharp", "Attack too soft", "Too buzzy",
      "Too synthetic", "Too much movement", "Disappears in mix",
    ];
    const reactionRoot = document.querySelector("#lab-reactions");
    for (const reaction of reactions) {
      const button = document.createElement("button");
      button.textContent = reaction;
      button.addEventListener("click", () => appendReaction(reaction));
      reactionRoot.append(button);
    }
    renderSnapshots();
  }

  function bindEvents() {
    document.querySelector("#show-style-workbench").addEventListener("click", () => showView(false));
    document.querySelector("#show-instrument-lab").addEventListener("click", () => showView(true));
    profileSelect.addEventListener("change", () => loadProfile(profileSelect.value));
    roleSelect.addEventListener("change", () => loadRole(roleSelect.value));
    sourceSelect.addEventListener("change", () => {
      patch.source = sourceSelect.value;
      enforceJam2Audition();
      rawPatchChanged();
      renderParameterGroups();
    });
    sourceBSelect.addEventListener("change", () => {
      patch.secondSource = sourceBSelect.value;
      if (patch.secondSource === "off") patch.sourceBlend = 0;
      enforceJam2Audition();
      rawPatchChanged();
    });
    document.querySelector("#lab-source-blend").addEventListener("input", event => {
      patch.sourceBlend = Number(event.target.value);
      rawPatchChanged(false);
      updateBlendLabels();
    });
    document.querySelector("#lab-source-blend").addEventListener("change", scheduleRender);
    document.querySelector("#lab-source-transpose").addEventListener("input", event => {
      patch.secondSourceTranspose = Number(event.target.value);
      rawPatchChanged(false);
    });
    document.querySelector("#lab-source-transpose").addEventListener("change", scheduleRender);
    document.querySelector("#lab-source-detune").addEventListener("input", event => {
      patch.secondSourceDetuneCents = Number(event.target.value);
      rawPatchChanged(false);
    });
    document.querySelector("#lab-source-detune").addEventListener("change", scheduleRender);
    filterSelect.addEventListener("change", () => {
      patch.filterArchitecture = filterSelect.value;
      rawPatchChanged();
    });
    auditionSelect.addEventListener("change", () => {
      enforceJam2Audition();
      setMixButtonsDisabled(false);
      scheduleRender();
    });
    rootSelect.addEventListener("change", scheduleRender);
    renderButton.addEventListener("click", renderAndPlay);
    replayButton.addEventListener("click", replay);
    mixButton.addEventListener("click", playInMix);
    jam2BackingButton.addEventListener(
      "click", () => playDrumsWithJam2Backing(false));
    jam2DrumsButton.addEventListener("click", playJam2DrumsOnly);
    jam2ReferenceButton.addEventListener(
      "click", () => playDrumsWithJam2Backing(true));
    document.querySelector("#lab-stop").addEventListener("click", stopAll);
    document.querySelector("#lab-reset-patch").addEventListener("click", () => {
      patch = clone(researchedPatch);
      rebaseMacros();
      persist();
      renderAllPatchControls();
      scheduleRender();
    });
    document.querySelector("#lab-reset-macros").addEventListener("click", () => {
      patch = clone(macroBase);
      resetMacroValues();
      persist();
      renderAllPatchControls();
      scheduleRender();
    });
    for (const input of document.querySelectorAll("[data-macro]")) {
      input.addEventListener("input", () => {
        macroValues[input.dataset.macro] = Number(input.value) / 100;
        input.closest("label").querySelector("output").textContent =
          Number(input.value) === 0 ? "Centre" : `${Number(input.value) > 0 ? "+" : ""}${input.value}`;
        applyMacros();
      });
      input.addEventListener("change", scheduleRender);
    }
    for (const button of document.querySelectorAll("[data-mutate]")) {
      button.addEventListener("click", () => mutate(button.dataset.mutate));
    }
    document.querySelector("#lab-notes").addEventListener("input", event => {
      stateForCurrent().notes = event.target.value;
      persist();
    });
    document.querySelector("#lab-reset-kit").addEventListener("click", () => {
      patch = clone(researchedPatch);
      drumKitCandidate =
        role.kitCandidates?.find(candidate => candidate.recommended)?.id ||
        role.kitCandidates?.[0]?.id || "";
      persist();
      renderAllPatchControls();
      scheduleRender();
    });
    document.querySelector("#lab-drum-kit-candidate").addEventListener("change", event => {
      const candidate = role.kitCandidates?.find(
        item => item.id === event.target.value);
      if (!candidate) {
        drumKitCandidate = "custom";
        return;
      }
      patch = normalizeKit(candidate.parameters, role.parameters);
      drumKitCandidate = candidate.id;
      persist();
      renderDrumControls();
      scheduleRender();
    });
    document.querySelector("#lab-drum-piece-candidate").addEventListener("change", event => {
      const candidate = role.kitCandidates?.find(
        item => item.id === event.target.value);
      if (!candidate) return;
      const resolved = normalizeKit(candidate.parameters, role.parameters);
      patch.pieces[drumPiece] = clone(resolved.pieces[drumPiece]);
      markDrumCustom();
      persist();
      renderDrumControls();
      scheduleRender();
    });
    document.querySelector("#lab-drum-source").addEventListener("change", event => {
      currentDrumPatch().source = event.target.value;
      markDrumCustom();
      persist();
      renderDrumControls();
      scheduleRender();
    });
    document.querySelector("#lab-drum-source-b").addEventListener("change", event => {
      currentDrumPatch().secondSource = event.target.value;
      if (event.target.value === "off") currentDrumPatch().blend = 0;
      markDrumCustom();
      persist();
      renderDrumControls();
      scheduleRender();
    });
    document.querySelector("#lab-drum-blend").addEventListener("input", event => {
      currentDrumPatch().blend = Number(event.target.value);
      markDrumCustom();
      persist();
      updateBlendLabels();
    });
    document.querySelector("#lab-drum-blend").addEventListener("change", scheduleRender);
    document.querySelector("#lab-drum-synth-source").addEventListener("change", event => {
      currentDrumPatch().synthLayer.source = event.target.value;
      markDrumCustom();
      persist();
      renderDrumControls();
      scheduleRender();
    });
    document.querySelector("#lab-drum-transient").addEventListener("change", event => {
      currentDrumPatch().transient.type = event.target.value;
      markDrumCustom();
      persist();
      renderDrumControls();
      scheduleRender();
    });
    document.querySelector("#lab-drum-texture").addEventListener("change", event => {
      currentDrumPatch().texture.type = event.target.value;
      markDrumCustom();
      persist();
      renderDrumControls();
      scheduleRender();
    });
    document.querySelector("#lab-save-review").addEventListener("click", saveForReview);
    document.querySelector("#lab-copy-json").addEventListener("click", async () => {
      await navigator.clipboard.writeText(JSON.stringify(soundDesignRecord(), null, 2));
      document.querySelector("#lab-save-review-status").textContent = "Copied JSON.";
    });
  }

  function showView(showLab) {
    stopAll();
    lab.hidden = !showLab;
    workbench.hidden = showLab;
    document.body.classList.toggle("lab-mode", showLab);
    document.querySelector("#show-instrument-lab").classList.toggle("active", showLab);
    document.querySelector("#show-style-workbench").classList.toggle("active", !showLab);
  }

  async function checkService() {
    const label = document.querySelector("#lab-service");
    try {
      const response = await fetch("/api/status", {cache: "no-store"});
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error("Native renderer unavailable");
      label.textContent = `Native renderer ready · ${manifest.sampleRate / 1000} kHz WAV`;
      label.classList.add("ready");
    } catch (error) {
      label.textContent = "Native renderer unavailable — launch with open-workbench.cmd";
      label.classList.add("error");
      renderButton.disabled = true;
      setMixButtonsDisabled(true);
    }
  }

  function selectInitialProfile() {
    const query = new URLSearchParams(location.search);
    const requestedProfile = query.get("profile");
    const profileId = manifest.profiles.some(item => item.id === requestedProfile)
      ? requestedProfile : saved.profileId;
    const valid = manifest.profiles.some(item => item.id === profileId);
    const selected = valid ? profileId : manifest.profiles[0].id;
    const requestedRole = query.get("role");
    if (labRoles.has(requestedRole)) {
      saved.roleByProfile ||= {};
      saved.roleByProfile[selected] = requestedRole;
    }
    loadProfile(selected);
    if (query.get("view") === "lab") showView(true);
  }

  function loadProfile(profileId) {
    profile = manifest.profiles.find(item => item.id === profileId) || manifest.profiles[0];
    saved.profileId = profile.id;
    profileSelect.value = profile.id;
    roleSelect.replaceChildren();
    for (const candidate of profile.roles.filter(item => labRoles.has(item.id))) {
      roleSelect.add(new Option(candidate.name, candidate.id));
    }
    const wanted = saved.roleByProfile?.[profile.id];
    const available = [...roleSelect.options].some(option => option.value === wanted);
    loadRole(available ? wanted : roleSelect.options[0].value);
    document.querySelector("#lab-sound-target").textContent = profile.soundBrief;
    persist();
  }

  function loadRole(roleId) {
    role = profile.roles.find(item => item.id === roleId && labRoles.has(item.id))
      || profile.roles.find(item => labRoles.has(item.id));
    saved.roleByProfile ||= {};
    saved.roleByProfile[profile.id] = role.id;
    roleSelect.value = role.id;
    const state = stateForCurrent();
    researchedPatch = role.id === "drums"
      ? normalizeKit(role.parameters)
      : normalizePatch(role.parameters);
    patch = role.id === "drums"
      ? normalizeKit(researchedPatch)
      : normalizePatch(state.patch || researchedPatch);
    drumKitCandidate = role.id === "drums"
      ? (role.kitCandidates?.find(candidate => candidate.recommended)?.id ||
          role.kitCandidates?.[0]?.id || "")
      : "";
    macroBase = clone(patch);
    resetMacroValues();
    rootSelect.value = String(role.id === "bass" ? 40 : role.id === "chords" ? 48 : 60);
    document.querySelector("#lab-design-name").textContent =
      `${role.designName} · ${role.targetPatchId}`;
    document.querySelector("#lab-notes").value = state.notes || "";
    currentUrl = "";
    lastRenderDiagnostics = null;
    replayButton.disabled = true;
    const drumRole = role.id === "drums";
    jam2BackingButton.hidden = !drumRole;
    jam2DrumsButton.hidden = !drumRole;
    jam2ReferenceButton.hidden = !drumRole;
    setMixButtonsDisabled(false);
    renderStatus.textContent = "No custom render yet.";
    configureAuditions();
    renderAllPatchControls();
    renderSnapshots();
    persist();
  }

  function renderAllPatchControls() {
    const drums = role?.id === "drums";
    for (const section of document.querySelectorAll("[data-pitched-only]")) {
      section.hidden = drums;
    }
    document.querySelector("#drum-kit-lab").hidden = !drums;
    rootSelect.closest("label").hidden = drums;
    if (drums) {
      renderDrumControls();
      updateRawJson();
      return;
    }
    sourceSelect.value = patch.source;
    sourceBSelect.value = patch.secondSource;
    document.querySelector("#lab-source-blend").value = String(patch.sourceBlend);
    document.querySelector("#lab-source-transpose").value = String(patch.secondSourceTranspose);
    document.querySelector("#lab-source-detune").value = String(patch.secondSourceDetuneCents);
    filterSelect.value = patch.filterArchitecture;
    renderParameterGroups();
    updateBlendLabels();
    updateRawJson();
  }

  function renderParameterGroups() {
    const root = document.querySelector("#lab-parameter-groups");
    root.replaceChildren();
    const activeSource = patch.source === "jam2-native"
      ? patch.secondSource : patch.source;
    const visible = parameters.filter(parameter =>
      !parameter.sources || parameter.sources.includes(activeSource)
    );
    const groups = [...new Set(visible.map(parameter => parameter.group))];
    for (const group of groups) {
      const section = document.createElement("section");
      section.className = "parameter-group";
      section.innerHTML = `<h4>${group}</h4><div class="parameter-grid"></div>`;
      const grid = section.querySelector(".parameter-grid");
      for (const parameter of visible.filter(item => item.group === group)) {
        const value = clampValue(parameter, patch[parameter.key]);
        patch[parameter.key] = value;
        const control = document.createElement("label");
        control.className = "parameter-control";
        control.innerHTML = `
          <span>${parameter.label}</span>
          <input type="range" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${value}">
          <input class="number" type="number" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${formatNumber(value)}">
          <small>${parameter.unit || ""}</small>
        `;
        const range = control.querySelector('input[type="range"]');
        const number = control.querySelector('input[type="number"]');
        const update = source => {
          const next = clampValue(parameter, Number(source.value));
          patch[parameter.key] = next;
          range.value = String(next);
          number.value = formatNumber(next);
          rawPatchChanged(false);
        };
        range.addEventListener("input", () => update(range));
        number.addEventListener("input", () => {
          if (number.value !== "") update(number);
        });
        range.addEventListener("change", scheduleRender);
        number.addEventListener("change", scheduleRender);
        grid.append(control);
      }
      root.append(section);
    }
  }

  function rawPatchChanged(renderControls = true) {
    macroBase = clone(patch);
    resetMacroValues();
    persist();
    updateRawJson();
    if (renderControls) renderAllPatchControls();
  }

  function applyMacros() {
    patch = clone(macroBase);
    const hardness = macroValues.hardness;
    patch.attackSeconds *= Math.pow(2, -3 * hardness);
    patch.transientMix += Math.max(0, hardness) * .14;
    patch.stringDamping += Math.max(0, hardness) * .18;

    const brightness = macroValues.brightness;
    patch.filterCutoffHz *= Math.pow(2, 2.2 * brightness);
    patch.stringBrightness += .34 * brightness;
    patch.spectralShape += .25 * brightness;

    const body = macroValues.body;
    patch.subMix += .35 * body;
    patch.oscillator2Mix += .18 * body;
    patch.filterCutoffHz *= Math.pow(2, -.35 * body);

    const grit = macroValues.grit;
    patch.voiceDrive *= Math.pow(2, 1.8 * grit);
    patch.busDrive *= Math.pow(2, 1.25 * grit);
    patch.filterDrive *= Math.pow(2, 1.1 * grit);
    patch.wavefold += Math.max(0, grit) * 2.2;

    const movement = macroValues.movement;
    patch.vibratoCents += Math.max(0, movement) * 13;
    patch.tremoloDepth += Math.max(0, movement) * .22;
    patch.chorusMix += Math.max(0, movement) * .25;
    if (movement < 0) {
      patch.vibratoCents *= 1 + movement;
      patch.tremoloDepth *= 1 + movement;
      patch.chorusMix *= 1 + movement;
    }

    const length = macroValues.length;
    patch.decaySeconds *= Math.pow(2, 2 * length);
    patch.releaseSeconds *= Math.pow(2, 2.5 * length);
    patch.sustain += .22 * length;

    const space = macroValues.space;
    patch.delayMix += Math.max(0, space) * .34;
    patch.chorusMix += Math.max(0, space) * .16;
    if (space < 0) {
      patch.delayMix *= 1 + space;
      patch.chorusMix *= 1 + space;
    }
    for (const parameter of parameters) {
      patch[parameter.key] = clampValue(parameter, patch[parameter.key]);
    }
    persist();
    renderAllPatchControls();
  }

  function mutate(kind) {
    if (kind === "source") {
      const alternatives = sourceOptions.map(([id]) => id).filter(id => id !== patch.source);
      patch.source = alternatives[Math.floor(Math.random() * alternatives.length)];
      enforceJam2Audition();
    } else {
      const keys = kind === "envelope"
        ? ["attackSeconds", "decaySeconds", "sustain", "releaseSeconds", "transientMix"]
        : kind === "motion"
          ? ["vibratoCents", "vibratoRateHz", "tremoloDepth", "tremoloRateHz", "chorusMix", "chorusDepth"]
          : ["shape", "width", "fmRatio", "fmIndex", "spectralShape", "stringBrightness", "filterCutoffHz", "resonance", "voiceDrive"];
      for (const key of keys) {
        const definition = parameterMap.get(key);
        if (!definition || (definition.sources && !definition.sources.includes(patch.source))) continue;
        const span = definition.max - definition.min;
        patch[key] = clampValue(definition, patch[key] + span * (Math.random() - .5) * .24);
      }
    }
    rawPatchChanged();
    scheduleRender();
  }

  async function renderAndPlay() {
    stopAll();
    renderStatus.classList.remove("error");
    if (renderAbort) renderAbort.abort();
    renderAbort = new AbortController();
    renderButton.disabled = true;
    renderStatus.textContent = "Rendering native DaisySP WAV…";
    const request = {
      schema: "jam2-instrument-patch-v1",
      profileId: profile.id,
      role: role.id,
      audition: auditionSelect.value,
      rootMidi: Number(rootSelect.value),
    };
    if (role.id === "drums") {
      request.kit = patch;
      request.drumPiece = drumPiece;
    } else {
      request.patch = patch;
    }
    try {
      const response = await fetch("/api/render", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(request),
        signal: renderAbort.signal,
      });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || "Render failed.");
      currentUrl = `${result.url}?v=${Date.now()}`;
      lastRenderDiagnostics = role.id === "drums"
        ? result.native || null : null;
      updateRawJson();
      replayButton.disabled = false;
      setMixButtonsDisabled(false);
      const events = result.native?.events;
      const seconds = result.native?.frames
        ? (result.native.frames / manifest.sampleRate).toFixed(2)
        : "";
      renderStatus.textContent = `${result.native?.cached ? "Loaded cached" : "Rendered"} WAV${events ? ` · ${events} events` : ""}${seconds ? ` · ${seconds} s` : ""}`;
      if (result.native?.productionPatternExact) {
        renderStatus.textContent +=
          ` · exact Jam2 performance seed ${result.native.patternSeed}` +
          ` · ${result.native.fillEvents} fill hits` +
          ` · ${result.native.microtimedEvents} microtimed hits`;
      }
      if (result.native?.injectedAuditionHit) {
        renderStatus.textContent +=
          " · added one audition hit because this generated groove does not use the selected piece";
      }
      replay();
    } catch (error) {
      if (error.name !== "AbortError") {
        renderStatus.textContent = error.message;
        renderStatus.classList.add("error");
      }
    } finally {
      renderButton.disabled = false;
    }
  }

  function replay() {
    stopAll();
    if (!currentUrl) return;
    soloAudio = new Audio(currentUrl);
    soloAudio.play().catch(error => {
      renderStatus.textContent = `Playback failed: ${error.message}`;
    });
  }

  function activeMixRequest(candidateRole) {
    const key = `${profile.id}/${candidateRole.id}`;
    const active = saved.patches?.[key]?.patch;
    const request = {
      schema: "jam2-instrument-patch-v1",
      profileId: profile.id,
      role: candidateRole.id,
      audition: "profile",
      rootMidi: candidateRole.id === "bass"
        ? 40 : candidateRole.id === "chords" ? 48 : 60,
    };
    if (candidateRole.id === "drums") {
      request.drumPiece = drumPiece;
      request.kit = normalizeKit(
        active || candidateRole.parameters,
        candidateRole.parameters);
    } else {
      request.patch = normalizePatch(
        active || candidateRole.parameters);
    }
    return request;
  }

  async function playInMix() {
    if (!profile || !role) return;
    stopAll();
    renderStatus.classList.remove("error");
    setMixButtonsDisabled(true);
    renderStatus.textContent = "Loading sample-aligned style mix…";
    try {
      persist();
      renderStatus.textContent =
        `Rendering ${profile.roles.length} currently active profile roles…`;
      const entries = await Promise.all(profile.roles.map(async candidateRole => {
        const request = activeMixRequest(candidateRole);
        const response = await fetch("/api/render", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(request),
        });
        const result = await response.json();
        if (!response.ok || !result.ok) {
          throw new Error(
            `${candidateRole.name}: ${result.error || "render failed."}`);
        }
        return {
          role: candidateRole,
          url: `${result.url}?v=${Date.now()}`,
        };
      }));
      await playAlignedEntries(entries);
      renderStatus.textContent =
        "Playing every role from its currently active sample-aligned patch.";
    } catch (error) {
      renderStatus.textContent = `Mix playback failed: ${error.message}`;
      renderStatus.classList.add("error");
    } finally {
      setMixButtonsDisabled(false);
    }
  }

  function jam2Entry(candidateRole) {
    const candidate =
      candidateRole.candidates?.find(item => item.id === "jam2");
    if (!candidate?.path) {
      throw new Error(`${candidateRole.name}: Jam2 reference is unavailable.`);
    }
    return {
      role: candidateRole,
      url: `${candidate.path}?v=${encodeURIComponent(manifest.generatedAt)}`,
    };
  }

  async function renderActiveDrumsEntry() {
    const drumRole = profile.roles.find(item => item.id === "drums");
    if (!drumRole) throw new Error("This profile has no drum role.");
    const response = await fetch("/api/render", {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify(activeMixRequest(drumRole)),
    });
    const result = await response.json();
    if (!response.ok || !result.ok) {
      throw new Error(`Edited drums: ${result.error || "render failed."}`);
    }
    return {
      role: drumRole,
      url: `${result.url}?v=${Date.now()}`,
    };
  }

  async function playDrumsWithJam2Backing(referenceDrums) {
    if (!profile || role?.id !== "drums") return;
    stopAll();
    renderStatus.classList.remove("error");
    setMixButtonsDisabled(true);
    renderStatus.textContent = referenceDrums
      ? "Loading Jam2 drums with fixed Jam2 backing…"
      : "Rendering edited drums with fixed Jam2 backing…";
    try {
      persist();
      const entries = await Promise.all(profile.roles.map(
        async candidateRole => {
          if (candidateRole.id !== "drums" || referenceDrums) {
            return jam2Entry(candidateRole);
          }
          return renderActiveDrumsEntry();
        }));
      await playAlignedEntries(entries);
      renderStatus.textContent = referenceDrums
        ? "Playing Jam2 drums and Jam2 backing: the fixed A/B reference."
        : "Playing edited drums against the same fixed Jam2 backing.";
    } catch (error) {
      renderStatus.textContent = `Jam2 A/B playback failed: ${error.message}`;
      renderStatus.classList.add("error");
    } finally {
      setMixButtonsDisabled(false);
    }
  }

  function playJam2DrumsOnly() {
    if (!profile || role?.id !== "drums") return;
    stopAll();
    renderStatus.classList.remove("error");
    try {
      const drumRole = profile.roles.find(item => item.id === "drums");
      if (!drumRole) throw new Error("This profile has no drum role.");
      soloAudio = new Audio(jam2Entry(drumRole).url);
      soloAudio.play().catch(error => {
        renderStatus.textContent =
          `Jam2 drum playback failed: ${error.message}`;
        renderStatus.classList.add("error");
      });
      renderStatus.textContent =
        "Playing the isolated Jam2 drums for the same generated pattern.";
    } catch (error) {
      renderStatus.textContent =
        `Jam2 drum playback failed: ${error.message}`;
      renderStatus.classList.add("error");
    }
  }

  async function playAlignedEntries(entries) {
    audioContext ||= new (window.AudioContext || window.webkitAudioContext)();
    await audioContext.resume();
    const buffers = await Promise.all(entries.map(async entry => {
      const response = await fetch(entry.url, {cache: "no-store"});
      if (!response.ok) throw new Error(`Could not load ${entry.role.name}.`);
      return audioContext.decodeAudioData(await response.arrayBuffer());
    }));
    const master = audioContext.createGain();
    master.gain.value = .42;
    master.connect(audioContext.destination);
    const start = audioContext.currentTime + .08;
    mixSources = buffers.map((buffer, index) => {
      const source = audioContext.createBufferSource();
      const gain = audioContext.createGain();
      gain.gain.value =
        mixRoleGains[entries[index].role.id] ?? mixRoleGains.default;
      source.buffer = buffer;
      source.connect(gain).connect(master);
      source.start(start);
      return source;
    });
  }

  function setMixButtonsDisabled(disabled) {
    mixButton.disabled = disabled;
    jam2BackingButton.disabled = disabled;
    jam2DrumsButton.disabled = disabled;
    jam2ReferenceButton.disabled = disabled;
  }

  function stopAll() {
    if (soloAudio) {
      soloAudio.pause();
      soloAudio.currentTime = 0;
      soloAudio = null;
    }
    for (const source of mixSources) {
      try { source.stop(); } catch {}
    }
    mixSources = [];
  }

  function scheduleRender() {
    persist();
    if (!document.querySelector("#lab-auto-render").checked) return;
    clearTimeout(autoRenderTimer);
    autoRenderTimer = setTimeout(renderAndPlay, 650);
  }

  function renderSnapshots() {
    const root = document.querySelector("#lab-snapshots");
    if (!root || !profile || !role) return;
    root.replaceChildren();
    const state = stateForCurrent();
    state.snapshots ||= {};
    for (const slot of ["A", "B", "C"]) {
      const wrapper = document.createElement("div");
      wrapper.className = "snapshot";
      wrapper.innerHTML = `
        <strong>${slot}</strong>
        <span>${state.snapshots[slot]
          ? (role.id === "drums"
              ? "Complete kit"
              : sourceLabel(state.snapshots[slot].source))
          : "Researched default"}</span>
        <button data-save>Save</button>
        <button data-load>Load</button>
      `;
      wrapper.querySelector("[data-save]").addEventListener("click", () => {
        state.snapshots[slot] = clone(patch);
        persist();
        renderSnapshots();
      });
      wrapper.querySelector("[data-load]").addEventListener("click", () => {
        patch = clone(state.snapshots[slot] || researchedPatch);
        rebaseMacros();
        persist();
        renderAllPatchControls();
        scheduleRender();
      });
      root.append(wrapper);
    }
  }

  function appendReaction(text) {
    const notes = document.querySelector("#lab-notes");
    const prefix = notes.value.trim() ? "\n" : "";
    notes.value += `${prefix}${text}.`;
    stateForCurrent().notes = notes.value;
    persist();
  }

  function stateForCurrent() {
    saved.patches ||= {};
    if (!profile || !role) return {};
    const key = `${profile.id}/${role.id}`;
    saved.patches[key] ||= {};
    return saved.patches[key];
  }

  function persist() {
    if (profile && role && patch) stateForCurrent().patch = clone(patch);
    localStorage.setItem(storageKey, JSON.stringify(saved));
    updateRawJson();
  }

  function loadSaved() {
    try {
      return JSON.parse(localStorage.getItem(storageKey)) || {};
    } catch {
      return {};
    }
  }

  function normalizeKit(
      source,
      defaultParameters = role?.parameters) {
    const busSource = source.bus || {};
    const next = {
      candidateId: source.candidateId || "custom",
      candidateName: source.candidateName || "Custom",
      recommended: Boolean(source.recommended),
      description: source.description || "",
      researchFamily: source.researchFamily || "custom",
      sourceReferences: Array.isArray(source.sourceReferences)
        ? [...source.sourceReferences] : [],
      pieces: {},
      bus: {},
    };
    const busFallbacks = {
      drive: 1.24,
      lowpassHz: 14500,
      compressorThreshold: .12,
      compressorRatio: 2.4,
      compressorReleaseMs: 55,
      roomMix: .08,
      roomSizeMs: 31,
      roomDamping: .58,
    };
    for (const parameter of drumBusParameters) {
      next.bus[parameter.key] = clamp(
        busSource[parameter.key] ?? busFallbacks[parameter.key],
        parameter.min,
        parameter.max);
    }
    for (const [id] of drumPieces) {
      const legacyTom =
        id === "mid-tom" ? source.pieces?.tom : undefined;
      const candidate = source.pieces?.[id] || legacyTom || {};
      const validSource = drumSourceOptions.some(([value]) => value === candidate.source);
      const validSecond = candidate.secondSource === "off" ||
        drumSourceOptions.some(([value]) => value === candidate.secondSource);
      const defaults =
        defaultParameters?.pieces?.[id] ||
        (id === "mid-tom"
          ? defaultParameters?.pieces?.tom
          : undefined) ||
        {};
      next.pieces[id] = {
        intendedIdentity: String(
          candidate.intendedIdentity ??
          defaults.intendedIdentity ??
          drumPieces.find(([pieceId]) => pieceId === id)?.[1] ??
          id).slice(0, 96),
        source: validSource ? candidate.source : (defaults.source || "daisy-profile"),
        secondSource: validSecond ? candidate.secondSource : (defaults.secondSource || "off"),
        blend: clamp(candidate.blend ?? defaults.blend ?? 0, 0, 1),
        frequencyHz: clamp(candidate.frequencyHz ?? defaults.frequencyHz ?? 180, 20, 12000),
        tone: clamp(candidate.tone ?? defaults.tone ?? .5, 0, 1),
        decay: clamp(candidate.decay ?? defaults.decay ?? .35, .001, 1),
        colour: clamp(candidate.colour ?? defaults.colour ?? .6, 0, 1),
        fmAmount: clamp(candidate.fmAmount ?? defaults.fmAmount ?? .3, 0, 1),
        level: clamp(candidate.level ?? defaults.level ?? .45, 0, 1.5),
        transient: normalizeDrumObject(
          candidate.transient,
          defaults.transient,
          drumComponentParameters.filter(parameter =>
            parameter.object === "transient"),
          {type: "off", level: 0, tone: .5, decaySeconds: .014}),
        texture: normalizeDrumObject(
          candidate.texture,
          defaults.texture,
          drumComponentParameters.filter(parameter =>
            parameter.object === "texture"),
          {type: "off", level: 0, tone: .5, decaySeconds: .18, density: .5}),
        colourStage: normalizeDrumObject(
          candidate.colourStage,
          defaults.colourStage,
          drumComponentParameters.filter(parameter =>
            parameter.object === "colourStage"),
          {voiceDrive: 1, sampleRateHz: 48000, bitDepth: 24,
            reconstructionLowpassHz: 20000, dynamicFilterAmount: 0}),
        roomSend: clamp(candidate.roomSend ?? defaults.roomSend ?? .08, 0, 1),
        relationship: {
          chokeGroup: String(
            candidate.relationship?.chokeGroup ??
            defaults.relationship?.chokeGroup ?? ""),
          chokeSeconds: clamp(
            candidate.relationship?.chokeSeconds ??
            defaults.relationship?.chokeSeconds ?? .012,
            .001, .25),
        },
        velocity: normalizeDrumVelocity(
          candidate.velocity,
          defaults.velocity),
        synthLayer: normalizeDrumSynthLayer(
          candidate.synthLayer || {},
          defaults.synthLayer || {}),
      };
    }
    return next;
  }

  function normalizeDrumObject(source = {}, defaults = {}, definitions, fallbacks) {
    const next = {};
    if ("type" in fallbacks) next.type = source.type ?? defaults.type ?? fallbacks.type;
    for (const parameter of definitions) {
      const fallback = fallbacks[parameter.key];
      let value = clamp(
        source?.[parameter.key] ??
          defaults?.[parameter.key] ?? fallback,
        parameter.min,
        parameter.max);
      if (parameter.integer) value = Math.round(value);
      next[parameter.key] = value;
    }
    return next;
  }

  function normalizeDrumVelocity(source = {}, defaults = {}) {
    const fallback = {
      ghost: {minimum: 18, maximum: 46},
      normal: {minimum: 54, maximum: 96},
      accent: {minimum: 92, maximum: 124},
      excitationCurve: .72,
      outputCurve: 1.35,
      brightnessAmount: .16,
      decayAmount: .08,
      driveAmount: .1,
    };
    const next = {};
    for (const band of ["ghost", "normal", "accent"]) {
      const sourceBand = source[band] || {};
      const defaultBand = defaults[band] || {};
      next[band] = {
        minimum: Math.round(clamp(
          sourceBand.minimum ?? defaultBand.minimum ??
            fallback[band].minimum, 1, 127)),
        maximum: Math.round(clamp(
          sourceBand.maximum ?? defaultBand.maximum ??
            fallback[band].maximum, 1, 127)),
      };
      next[band].maximum = Math.max(
        next[band].minimum, next[band].maximum);
    }
    for (const parameter of drumVelocityParameters.filter(
      item => !item.band)) {
      next[parameter.key] = clamp(
        source[parameter.key] ?? defaults[parameter.key] ??
          fallback[parameter.key],
        parameter.min,
        parameter.max);
    }
    return next;
  }

  function normalizeDrumSynthLayer(source, defaults) {
    const selected = drumSynthSourceOptions.some(
      ([value]) => value === source.source)
      ? source.source
      : (drumSynthSourceOptions.some(
          ([value]) => value === defaults.source)
          ? defaults.source : "off");
    const next = {source: selected};
    const fallbacks = {
      midiNote: 60,
      level: .22,
      gateSeconds: .08,
      attackSeconds: .001,
      decaySeconds: .06,
      sustain: .01,
      releaseSeconds: .08,
      noiseMix: 0,
      filterCutoffHz: 8000,
    };
    for (const parameter of drumSynthParameters) {
      next[parameter.key] = clamp(
        source[parameter.key] ??
          defaults[parameter.key] ??
          fallbacks[parameter.key],
        parameter.min,
        parameter.max);
      if (parameter.key === "midiNote") {
        next[parameter.key] = Math.round(next[parameter.key]);
      }
    }
    return next;
  }

  function currentDrumPatch() {
    return patch.pieces[drumPiece];
  }

  function configureAuditions() {
    auditionSelect.replaceChildren();
    if (role.id === "drums") {
      auditionSelect.add(new Option("One-shot", "one-shot"));
      auditionSelect.add(new Option(
        "Ghost / normal / accent ladder", "velocity-ladder"));
      auditionSelect.add(new Option(
        "Deterministic repeated hits", "repeated-hits"));
      auditionSelect.add(new Option("Selected piece in profile groove", "piece"));
      auditionSelect.add(new Option("Complete generated style groove", "profile"));
      auditionSelect.value = "one-shot";
    } else {
      auditionSelect.add(new Option("Single note", "note"));
      auditionSelect.add(new Option("Register + velocity", "velocity"));
      auditionSelect.add(new Option("Two polyphonic chords", "chord"));
      auditionSelect.add(new Option("Generated style phrase", "profile"));
      auditionSelect.value = "profile";
      enforceJam2Audition();
    }
  }

  function enforceJam2Audition() {
    if (role?.id === "drums") return;
    if (patch?.source === "jam2-native" ||
        patch?.secondSource === "jam2-native") {
      auditionSelect.value = "profile";
    }
  }

  function markDrumCustom() {
    if (role?.id !== "drums" || !patch) return;
    drumKitCandidate = "custom";
    patch.candidateId = "custom";
    patch.candidateName = "Custom";
    patch.recommended = false;
    patch.description =
      "Manually edited from researched component parameters.";
    patch.researchFamily = "custom";
    const kitSelect =
      document.querySelector("#lab-drum-kit-candidate");
    if (kitSelect) kitSelect.value = "custom";
    const pieceSelect =
      document.querySelector("#lab-drum-piece-candidate");
    if (pieceSelect) pieceSelect.value = "custom";
  }

  function appendDrumParameterControl(root, parameter, read, write) {
    let value = clamp(read(), parameter.min, parameter.max);
    if (parameter.integer) value = Math.round(value);
    write(value);
    const control = document.createElement("label");
    control.className = "parameter-control";
    control.innerHTML = `
      <span>${parameter.label}</span>
      <input type="range" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${value}">
      <input class="number" type="number" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${formatNumber(value)}">
      <small>${parameter.unit || ""}</small>
    `;
    const range = control.querySelector('input[type="range"]');
    const number = control.querySelector('input[type="number"]');
    const update = input => {
      let next = clamp(Number(input.value), parameter.min, parameter.max);
      if (parameter.integer) next = Math.round(next);
      write(next);
      markDrumCustom();
      range.value = String(next);
      number.value = formatNumber(next);
      persist();
    };
    range.addEventListener("input", () => update(range));
    number.addEventListener("input", () =>
      number.value !== "" && update(number));
    range.addEventListener("change", scheduleRender);
    number.addEventListener("change", scheduleRender);
    root.append(control);
  }

  function renderDrumControls() {
    drumKitCandidate = patch.candidateId || drumKitCandidate || "custom";
    const kitSelect =
      document.querySelector("#lab-drum-kit-candidate");
    kitSelect.replaceChildren();
    for (const candidate of role.kitCandidates || []) {
      kitSelect.add(new Option(
        `${candidate.recommended ? "Feedback focus - " : ""}${candidate.name}`,
        candidate.id));
    }
    kitSelect.add(new Option("Custom / candidate override off", "custom"));
    kitSelect.value = [...kitSelect.options].some(
      option => option.value === drumKitCandidate)
      ? drumKitCandidate : "custom";
    const tabs = document.querySelector("#lab-drum-pieces");
    tabs.replaceChildren();
    for (const [id, label] of drumPieces) {
      const button = document.createElement("button");
      button.textContent = label;
      button.classList.toggle("active", id === drumPiece);
      button.addEventListener("click", () => {
        drumPiece = id;
        renderDrumControls();
        if (auditionSelect.value === "piece") scheduleRender();
      });
      tabs.append(button);
    }
    const piece = currentDrumPatch();
    const pieceSelect =
      document.querySelector("#lab-drum-piece-candidate");
    pieceSelect.replaceChildren();
    for (const candidate of role.kitCandidates || []) {
      pieceSelect.add(new Option(
        `${candidate.recommended ? "Feedback focus - " : ""}${candidate.name} / ${drumPieces.find(([id]) => id === drumPiece)?.[1]}`,
        candidate.id));
    }
    pieceSelect.add(new Option("Custom", "custom"));
    const matchingPiece = (role.kitCandidates || []).find(candidate => {
      const candidateKit = normalizeKit(
        candidate.parameters, role.parameters);
      return JSON.stringify(candidateKit.pieces[drumPiece]) ===
        JSON.stringify(piece);
    });
    pieceSelect.value = matchingPiece?.id || "custom";
    const described = (role.kitCandidates || []).find(
      candidate => candidate.id ===
        (drumKitCandidate === "custom"
          ? pieceSelect.value : drumKitCandidate));
    const candidateDescription =
      described?.description || patch.description ||
      "Manual component combination.";
    document.querySelector("#lab-drum-candidate-description").textContent =
      `${piece.intendedIdentity || "Unspecified piece identity"} — ${candidateDescription}`;
    document.querySelector("#lab-drum-source").value = piece.source;
    document.querySelector("#lab-drum-source-b").value = piece.secondSource;
    document.querySelector("#lab-drum-blend").value = String(piece.blend);
    document.querySelector("#lab-drum-synth-source").value =
      piece.synthLayer.source;
    document.querySelector("#lab-drum-transient").value =
      piece.transient.type;
    document.querySelector("#lab-drum-texture").value =
      piece.texture.type;
    const root = document.querySelector("#lab-drum-parameters");
    root.replaceChildren();
    for (const parameter of drumParameters) {
      const value = clamp(piece[parameter.key], parameter.min, parameter.max);
      piece[parameter.key] = value;
      const control = document.createElement("label");
      control.className = "parameter-control";
      control.innerHTML = `
        <span>${parameter.label}</span>
        <input type="range" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${value}">
        <input class="number" type="number" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${formatNumber(value)}">
        <small>${parameter.unit || ""}</small>
      `;
      const range = control.querySelector('input[type="range"]');
      const number = control.querySelector('input[type="number"]');
      const update = input => {
        const next = clamp(Number(input.value), parameter.min, parameter.max);
        piece[parameter.key] = next;
        markDrumCustom();
        range.value = String(next);
        number.value = formatNumber(next);
        persist();
      };
      range.addEventListener("input", () => update(range));
      number.addEventListener("input", () => number.value !== "" && update(number));
      range.addEventListener("change", scheduleRender);
      number.addEventListener("change", scheduleRender);
      root.append(control);
    }
    const componentRoot =
      document.querySelector("#lab-drum-component-parameters");
    componentRoot.replaceChildren();
    for (const parameter of drumComponentParameters) {
      appendDrumParameterControl(
        componentRoot,
        parameter,
        () => parameter.object
          ? piece[parameter.object][parameter.key]
          : piece[parameter.key],
        value => {
          if (parameter.object) {
            piece[parameter.object][parameter.key] = value;
          } else {
            piece[parameter.key] = value;
          }
        });
    }
    const velocityRoot =
      document.querySelector("#lab-drum-velocity-parameters");
    velocityRoot.replaceChildren();
    for (const parameter of drumVelocityParameters) {
      appendDrumParameterControl(
        velocityRoot,
        parameter,
        () => parameter.band
          ? piece.velocity[parameter.band][parameter.key]
          : piece.velocity[parameter.key],
        value => {
          if (parameter.band) {
            piece.velocity[parameter.band][parameter.key] = value;
            const band = piece.velocity[parameter.band];
            if (band.maximum < band.minimum) {
              if (parameter.key === "minimum") {
                band.maximum = band.minimum;
              } else {
                band.minimum = band.maximum;
              }
            }
          } else {
            piece.velocity[parameter.key] = value;
          }
        });
    }
    const synthRoot =
      document.querySelector("#lab-drum-synth-parameters");
    synthRoot.replaceChildren();
    for (const parameter of drumSynthParameters) {
      const value = clamp(
        piece.synthLayer[parameter.key],
        parameter.min,
        parameter.max);
      piece.synthLayer[parameter.key] =
        parameter.key === "midiNote" ? Math.round(value) : value;
      const control = document.createElement("label");
      control.className = "parameter-control";
      control.innerHTML = `
        <span>${parameter.label}</span>
        <input type="range" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${value}">
        <input class="number" type="number" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${formatNumber(value)}">
        <small>${parameter.unit || ""}</small>
      `;
      const range = control.querySelector('input[type="range"]');
      const number = control.querySelector('input[type="number"]');
      const update = input => {
        let next = clamp(
          Number(input.value),
          parameter.min,
          parameter.max);
        if (parameter.key === "midiNote") next = Math.round(next);
        piece.synthLayer[parameter.key] = next;
        markDrumCustom();
        range.value = String(next);
        number.value = formatNumber(next);
        persist();
      };
      range.addEventListener("input", () => update(range));
      number.addEventListener("input", () =>
        number.value !== "" && update(number));
      range.addEventListener("change", scheduleRender);
      number.addEventListener("change", scheduleRender);
      synthRoot.append(control);
    }
    const busRoot = document.querySelector("#lab-drum-bus-parameters");
    busRoot.replaceChildren();
    for (const parameter of drumBusParameters) {
      const value = clamp(patch.bus[parameter.key], parameter.min, parameter.max);
      patch.bus[parameter.key] = value;
      const control = document.createElement("label");
      control.className = "parameter-control";
      control.innerHTML = `
        <span>${parameter.label}</span>
        <input type="range" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${value}">
        <input class="number" type="number" min="${parameter.min}" max="${parameter.max}" step="${parameter.step}" value="${formatNumber(value)}">
        <small>${parameter.unit || ""}</small>
      `;
      const range = control.querySelector('input[type="range"]');
      const number = control.querySelector('input[type="number"]');
      const update = input => {
        const next = clamp(Number(input.value), parameter.min, parameter.max);
        patch.bus[parameter.key] = next;
        markDrumCustom();
        range.value = String(next);
        number.value = formatNumber(next);
        persist();
      };
      range.addEventListener("input", () => update(range));
      number.addEventListener("input", () => number.value !== "" && update(number));
      range.addEventListener("change", scheduleRender);
      number.addEventListener("change", scheduleRender);
      busRoot.append(control);
    }
    updateBlendLabels();
    updateRawJson();
  }

  function updateBlendLabels() {
    const pitchedBlend = document.querySelector("#lab-source-blend-value");
    if (pitchedBlend && patch && role?.id !== "drums") {
      pitchedBlend.textContent = patch.secondSource === "off"
        ? "Off" : `${Math.round(patch.sourceBlend * 100)}% B`;
    }
    const drumBlend = document.querySelector("#lab-drum-blend-value");
    if (drumBlend && patch && role?.id === "drums") {
      const piece = currentDrumPatch();
      drumBlend.textContent = piece.secondSource === "off"
        ? "Off" : `${Math.round(piece.blend * 100)}% B`;
    }
  }

  function soundDesignRecord(includeFullDiagnostics = true) {
    const diagnostics = role.id === "drums"
      ? (includeFullDiagnostics
          ? lastRenderDiagnostics
          : lastRenderDiagnostics
            ? {
                audition: lastRenderDiagnostics.audition,
                candidateId: lastRenderDiagnostics.candidateId,
                events: lastRenderDiagnostics.events,
                frames: lastRenderDiagnostics.frames,
                sampleRate: lastRenderDiagnostics.sampleRate,
                metrics: lastRenderDiagnostics.metrics,
              }
            : null)
      : null;
    return {
      schema: "jam2-sound-design-preset-v1",
      profileId: profile.id,
      styleId: profile.styleId,
      profileName: profile.name,
      role: role.id,
      targetPatchId: role.targetPatchId,
      designName: role.designName,
      researchTarget: profile.soundBrief,
      notes: stateForCurrent().notes || "",
      updatedAt: new Date().toISOString(),
      renderDiagnostics: diagnostics,
      [role.id === "drums" ? "kit" : "patch"]: clone(patch),
    };
  }

  async function saveForReview() {
    const status = document.querySelector("#lab-save-review-status");
    status.textContent = "Saving…";
    try {
      const response = await fetch("/api/preset", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(soundDesignRecord(false)),
      });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || "Save failed.");
      status.textContent = `Saved ${profile.name} / ${role.name} for review.`;
    } catch (error) {
      status.textContent = error.message;
    }
  }

  function normalizePatch(source) {
    const next = {};
    next.source = sourceOptions.some(([id]) => id === source.source)
      ? source.source : "variable-shape";
    next.secondSource = source.secondSource === "off" ||
      sourceOptions.some(([id]) => id === source.secondSource)
      ? (source.secondSource || "off") : "off";
    next.sourceBlend = Math.max(0, Math.min(1, Number(source.sourceBlend || 0)));
    next.secondSourceTranspose = Math.max(-24, Math.min(24,
      Math.round(Number(source.secondSourceTranspose || 0))));
    next.secondSourceDetuneCents = Math.max(-100, Math.min(100,
      Number(source.secondSourceDetuneCents || 0)));
    next.filterArchitecture = filterOptions.some(([id]) => id === source.filterArchitecture)
      ? source.filterArchitecture : "ladder-lowpass";
    for (const parameter of parameters) {
      const fallback = parameter.key === "delaySeconds" ? .23
        : parameter.key === "wavefold" ? 0
        : parameter.min;
      next[parameter.key] = clampValue(parameter, Number(source[parameter.key] ?? fallback));
    }
    return next;
  }

  function rebaseMacros() {
    macroBase = clone(patch);
    resetMacroValues();
  }

  function resetMacroValues() {
    macroValues = Object.fromEntries(macroDefinitions.map(([id]) => [id, 0]));
    for (const input of document.querySelectorAll("[data-macro]")) {
      input.value = "0";
      input.closest("label").querySelector("output").textContent = "Centre";
    }
  }

  function updateRawJson() {
    const output = document.querySelector("#lab-json");
    if (output && patch && profile && role) {
      output.textContent = JSON.stringify(soundDesignRecord(), null, 2);
    }
  }

  function clamp(value, minimum, maximum) {
    value = Number(value);
    if (!Number.isFinite(value)) value = minimum;
    return Number(Math.max(minimum, Math.min(maximum, value)).toFixed(6));
  }

  function clampValue(parameter, value) {
    if (!Number.isFinite(value)) value = parameter.min;
    value = Math.max(parameter.min, Math.min(parameter.max, value));
    if (parameter.step >= 1) value = Math.round(value / parameter.step) * parameter.step;
    return Number(value.toFixed(6));
  }

  function formatNumber(value) {
    return Math.abs(value) >= 100 ? value.toFixed(0)
      : Math.abs(value) >= 10 ? value.toFixed(2)
      : value.toFixed(3);
  }

  function sourceLabel(id) {
    return sourceOptions.find(([value]) => value === id)?.[1] || id;
  }

  function styleName(id) {
    return manifest.styles.find(style => style.id === id)?.name || id;
  }

  function noteName(midi) {
    const names = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"];
    return `${names[midi % 12]}${Math.floor(midi / 12) - 1}`;
  }

  function clone(value) {
    return JSON.parse(JSON.stringify(value));
  }
})();
