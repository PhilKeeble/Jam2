(() => {
  const manifest = window.JAM2_SOUND_DESIGN_MANIFEST;
  if (!manifest) return;
  const droneProfiles = new Set([
    "electronic_techno", "modal_atmospheric", "modal_groove",
  ]);
  for (const profile of manifest.profiles) {
    if (droneProfiles.has(profile.id)) {
      profile.roles = profile.roles.filter(item => item.id !== "support");
    }
  }
  const pageStatus = document.querySelector("#status");
  if (window.location.protocol === "file:") {
    pageStatus.classList.add("error");
    pageStatus.innerHTML = "<strong>Run open-workbench.cmd.</strong> The sound editor needs its local renderer.";
    return;
  }
  pageStatus?.remove();
  const templateLibrary = window.JAM2_BASE_SOUND_LIBRARY;
  const electronicDrumProfiles = new Set([
    "electronic_breakbeat", "electronic_house", "electronic_techno",
    "hiphop_boom_bap", "hiphop_trap", "jpop_anisong_rock",
    "jpop_idol_dance", "metal_modern_progressive",
    "rnb_contemporary_neosoul", "soul_classic_motown",
  ]);
  const styleDrumKitId = profileId =>
    electronicDrumProfiles.has(profileId) ? "electronic" : "acoustic";
  const canonicalDrumKitId = (kitId, profileId) => {
    if (kitId === "ableton-rock32") return "acoustic";
    if (kitId === "ableton-808") return "electronic";
    return kitId === "acoustic" || kitId === "electronic"
      ? kitId : styleDrumKitId(profileId);
  };

  const storageKey = "jam2-instrument-lab-v1";
  const jam2StyleChoiceId = "__jam2-native-style__";
  const jam2MixSource = "jam2-native";
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
  ];
  const drumSourceOptions = [
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
    ["jam2-hand-clap", "Jam2 multi-burst hand clap"],
    ["jam2-wood-block", "Jam2 wood block"],
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
  const profileMixRoleGains = {
    pop_loop: {bass: .73, chords: .755, melody: .428, support: .403, drums: .787},
    pop_sectional: {bass: .73, chords: .755, melody: .428, support: .403, drums: .787},
  };
  function defaultMixGainForRole(roleId) {
    return profileMixRoleGains[profile?.id]?.[roleId] ??
      mixRoleGains[roleId] ?? mixRoleGains.default;
  }
  function mixTrimDbForRole(roleId) {
    const value = Number(saved.patches?.[`${profile?.id}/${roleId}`]?.mixTrimDb ?? 0);
    return Math.max(-36, Math.min(12, Number.isFinite(value) ? value : 0));
  }
  function mixGainForRole(roleId) {
    return defaultMixGainForRole(roleId) *
      Math.pow(10, mixTrimDbForRole(roleId) / 20);
  }
  function formatMixTrimDb(value) {
    const rounded = Number(value).toFixed(1);
    return `${value > 0 ? "+" : ""}${rounded} dB`;
  }
  const drumParameters = [
    {key: "frequencyHz", label: "Pitch / centre frequency", min: 20, max: 12000, step: 1, unit: " Hz"},
    {key: "tone", label: "Tone", min: 0, max: 1, step: .01},
    {key: "decay", label: "Decay", min: .001, max: 1, step: .001},
    {key: "colour", label: "Noise / snap / metal colour", min: 0, max: 1, step: .01},
    {key: "fmAmount", label: "Pitch / FM sweep", min: 0, max: 1, step: .01},
    {key: "level", label: "Piece level", min: 0, max: 1.5, step: .01},
    {key: "sourceLayerGain", label: "Source / model layer level", min: 0, max: 2, step: .01},
    {key: "onsetSofteningSeconds", label: "Onset softening", min: 0, max: .1, step: .001, unit: " s"},
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
    {key: "stereoWidth", label: "Stereo room width", min: 0, max: 1, step: .01},
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
    {group: "Harmonics", key: "harmonicFamily", label: "Harmonic family", min: 0, max: 6, step: 1, sources: ["additive-harmonic"]},
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
    {group: "Amplitude", key: "velocitySensitivity", label: "Velocity to level", min: 0, max: 1, step: .01},
    {group: "Filter", key: "filterCutoffHz", label: "Cutoff", min: 40, max: 18000, step: 10, unit: " Hz"},
    {group: "Filter", key: "highpassCutoffHz", label: "High-pass cutoff", min: 0, max: 4000, step: 5, unit: " Hz"},
    {group: "Filter", key: "filterEnvelopeHz", label: "Envelope depth", min: -12000, max: 12000, step: 10, unit: " Hz"},
    {group: "Filter", key: "filterEnvelopeDecaySeconds", label: "Envelope decay (0 = amp)", min: 0, max: 5, step: .005, unit: " s"},
    {group: "Filter", key: "filterEnvelopeSustain", label: "Envelope sustain", min: 0, max: 1, step: .005},
    {group: "Filter", key: "filterKeyTracking", label: "Key tracking", min: 0, max: 2, step: .01},
    {group: "Filter", key: "filterVelocitySensitivity", label: "Velocity to filter", min: 0, max: 1, step: .01},
    {group: "Filter", key: "resonance", label: "Resonance", min: 0, max: .95, step: .01},
    {group: "Filter", key: "filterDrive", label: "Filter drive", min: .5, max: 8, step: .01},
    {group: "Character", key: "wavefold", label: "Wavefold", min: 0, max: 8, step: .01},
    {group: "Character", key: "noiseMix", label: "Noise", min: 0, max: 1, step: .01},
    {group: "Character", key: "transientMix", label: "Transient layer", min: 0, max: 1, step: .005},
    {group: "Character", key: "transientSeconds", label: "Transient length", min: .001, max: .25, step: .001, unit: " s"},
    {group: "Character", key: "voiceDrive", label: "Voice drive", min: .5, max: 10, step: .01},
    {group: "Character", key: "busDrive", label: "Bus drive", min: .5, max: 10, step: .01},
    {group: "Character", key: "cabinet", label: "Cabinet colour", min: 0, max: 1, step: .01},
    {group: "Movement", key: "glideSeconds", label: "Pitch glide", min: 0, max: 1.5, step: .005, unit: " s"},
    {group: "Movement", key: "pitchEnvelopeSemitones", label: "Pitch-envelope depth", min: -48, max: 48, step: .25, unit: " st"},
    {group: "Movement", key: "pitchEnvelopeSeconds", label: "Pitch-envelope decay", min: .001, max: 2, step: .001, unit: " s"},
    {group: "Movement", key: "pitchAttackGain", label: "Pitched attack emphasis", min: 0, max: 16, step: .05},
    {group: "Movement", key: "pitchAttackSeconds", label: "Pitched attack decay", min: .001, max: 1, step: .001, unit: " s"},
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
    {group: "Space", key: "reverbMix", label: "Reverb mix", min: 0, max: .85, step: .01},
    {group: "Space", key: "reverbSeconds", label: "Reverb decay", min: .1, max: 8, step: .05, unit: " s"},
    {group: "Space", key: "reverbDamping", label: "Reverb damping", min: 0, max: 1, step: .01},
    {group: "Space", key: "reverbPreDelaySeconds", label: "Reverb pre-delay", min: 0, max: .25, step: .001, unit: " s"},
    {group: "Space", key: "stereoSpread", label: "Stereo decorrelation", min: 0, max: 1, step: .01},
    {group: "Space", key: "stereoWidth", label: "Stereo width", min: 0, max: 2, step: .01},
  ];
  const parameterMap = new Map(parameters.map(parameter => [parameter.key, parameter]));

  const macroDefinitions = [
    ["hardness", "Blooming ↔ percussive impact"],
    ["brightness", "Muted ↔ brilliant / spectral"],
    ["body", "Narrow / light ↔ layered / massive"],
    ["grit", "Pure ↔ folded / driven / noisy"],
    ["movement", "Still ↔ glide / vibrato / tremolo"],
    ["length", "Clipped ↔ evolving sustain"],
    ["space", "Bone dry ↔ wide chorus / delay"],
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
  let baseInstrumentId = "";
  let baseKitId = "";
  let treatmentId = "neutral";
  let selectedInstrumentVariantId = "";
  let selectedDrumVariantId = "";
  let labMode = "instruments";

  function isBaseMode() {
    return labMode === "instruments" || labMode === "drums";
  }

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
    const treatmentSelect = document.querySelector("#lab-style-treatment");
    if (templateLibrary && treatmentSelect) {
      treatmentSelect.add(new Option("Neutral / base sound", "neutral"));
      for (const style of manifest.styles) {
        if (!templateLibrary.treatments[style.id]) continue;
        treatmentSelect.add(new Option(style.name, style.id));
      }
    }
    renderSnapshots();
  }

  function bindEvents() {
    document.querySelector("#show-base-instruments").addEventListener(
      "click", () => showView("instruments"));
    document.querySelector("#show-base-drums").addEventListener(
      "click", () => showView("drums"));
    document.querySelector("#show-instrument-lab").addEventListener("click", () => showView("mix"));
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
      if (event.target.value.startsWith("base:")) {
        const baseId = event.target.value.slice("base:".length);
        const resolved = kitTemplatePatch(baseId, role);
        if (!resolved) return;
        patch.pieces[drumPiece] = clone(resolved.pieces[drumPiece]);
        markDrumCustom();
        if (labMode === "mix") {
          baseKitId = "";
          stateForCurrent().baseKitId = "";
        }
        persist();
        renderDrumControls();
        scheduleRender();
        return;
      }
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
    document.querySelector("#lab-save-style-mix")?.addEventListener(
      "click", saveCompleteStyleMix);
    document.querySelector("#lab-copy-json").addEventListener("click", async () => {
      await navigator.clipboard.writeText(JSON.stringify(soundDesignRecord(), null, 2));
      document.querySelector("#lab-save-review-status").textContent = "Copied JSON.";
    });
    document.querySelector("#lab-style-treatment")?.addEventListener("change", event => {
      treatmentId = event.target.value;
      stateForCurrent().treatmentId = treatmentId;
      persist();
      renderBasePalette();
      document.querySelector("#lab-style-assignment-status").textContent =
        "Treatment selected. Apply the base sound to update this style-role patch; the pattern remains unchanged.";
    });
    document.querySelector("#lab-assign-base")?.addEventListener(
      "click", applySelectedStyleBase);
    document.querySelector("#lab-save-base")?.addEventListener(
      "click", saveCurrentBase);
    document.querySelector("#lab-load-saved-base")?.addEventListener(
      "click", loadSavedBase);
    document.querySelector("#lab-load-factory-base")?.addEventListener(
      "click", loadFactoryBase);
    document.querySelector("#lab-update-base")?.addEventListener(
      "click", updateCurrentBase);
    document.querySelector("#lab-base-instrument-choice")?.addEventListener(
      "change", event => applyInstrumentTemplate(event.target.value));
    document.querySelector("#lab-base-kit-choice")?.addEventListener(
      "change", event => applyKitTemplate(event.target.value));
    document.querySelector("#lab-base-drum-piece-choice")?.addEventListener(
      "change", event => selectBaseDrumPiece(event.target.value));
    document.querySelector("#lab-base-drum-option")?.addEventListener(
      "change", event => loadDrumVariant(event.target.value));
    document.querySelector("#lab-style-base")?.addEventListener(
      "change", applySelectedStyleBase);
  }

  function showView(view) {
    stopAll();
    const nextMode = ["instruments", "drums", "mix"].includes(view)
      ? view : "instruments";
    const modeChanged = labMode !== nextMode;
    labMode = nextMode;
    lab.hidden = false;
    workbench.hidden = true;
    document.body.classList.add("lab-mode");
    document.body.classList.toggle("base-sound-mode", isBaseMode());
    document.body.classList.toggle("instrument-sound-mode", labMode === "instruments");
    document.body.classList.toggle("drum-sound-mode", labMode === "drums");
    document.body.classList.toggle("style-mix-mode", labMode === "mix");
    document.querySelector("#show-base-instruments").classList.toggle(
      "active", labMode === "instruments");
    document.querySelector("#show-base-drums").classList.toggle(
      "active", labMode === "drums");
    document.querySelector("#show-instrument-lab").classList.toggle(
      "active", labMode === "mix");
    if (modeChanged) {
      const wantedRole = labMode === "drums"
        ? "drums"
        : labMode === "instruments"
          ? (saved.baseEditor?.instrumentRoleId ||
              (role?.id !== "drums" ? role?.id : "chords"))
          : (saved.roleByProfile?.[profile.id] ||
              (role?.id !== "drums" ? role?.id : "chords"));
      loadRole(wantedRole);
    } else {
      renderAllPatchControls();
    }
  }

  async function checkService() {
    const label = document.querySelector("#lab-service");
    try {
      const response = await fetch("/api/status", {cache: "no-store"});
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error("Native renderer unavailable");
      const styleMixReady = result.capabilities?.includes("style-mix-save");
      const saveButton = document.querySelector("#lab-save-style-mix");
      if (saveButton) saveButton.disabled = !styleMixReady;
      if (!styleMixReady) {
        const saveStatus = document.querySelector("#lab-save-style-mix-status");
        if (saveStatus) {
          saveStatus.textContent =
            "Older workbench server detected. Close it and reopen open-workbench.cmd to enable project saving.";
          saveStatus.classList.add("error");
        }
      }
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
    if (query.get("view") === "lab" || query.get("view") === "mix") {
      showView("mix");
    } else if (query.get("view") === "drums") {
      showView("drums");
    } else {
      showView("instruments");
    }
  }

  function loadProfile(profileId) {
    profile = manifest.profiles.find(item => item.id === profileId) || manifest.profiles[0];
    saved.profileId = profile.id;
    const styleSaveStatus = document.querySelector("#lab-save-style-mix-status");
    if (styleSaveStatus) styleSaveStatus.textContent = "";
    profileSelect.value = profile.id;
    roleSelect.replaceChildren();
    for (const candidate of profile.roles.filter(item => labRoles.has(item.id))) {
      roleSelect.add(new Option(candidate.name, candidate.id));
    }
    const wanted = labMode === "instruments"
      ? saved.baseEditor?.instrumentRoleId
      : labMode === "drums"
        ? "drums"
        : saved.roleByProfile?.[profile.id];
    const available = [...roleSelect.options].some(option => option.value === wanted);
    loadRole(available ? wanted : roleSelect.options[0].value);
    document.querySelector("#lab-sound-target").textContent = profile.soundBrief;
    persist(false);
  }

  function loadRole(roleId) {
    const requestedRole = labMode === "drums" ? "drums" : roleId;
    role = profile.roles.find(
      item => item.id === requestedRole && labRoles.has(item.id));
    if (!role || (labMode === "instruments" && role.id === "drums")) {
      role = profile.roles.find(
        item => labRoles.has(item.id) && item.id !== "drums") ||
        profile.roles.find(item => labRoles.has(item.id));
    }
    roleSelect.value = role.id;
    const state = stateForCurrent();
    if (!isBaseMode() && role.id === "drums") {
      state.mixSource = "";
      state.baseKitId = canonicalDrumKitId(state.baseKitId, profile.id);
      state.drumPieceOptions = {};
    }
    if (isBaseMode()) {
      saved.baseEditor ||= {selectionByRole: {}};
      saved.baseEditor.selectionByRole ||= {};
      treatmentId = "neutral";
      if (labMode === "drums") {
        saved.baseEditor.drumPiece ||= "kick";
        drumPiece = saved.baseEditor.drumPiece;
        baseKitId = canonicalDrumKitId(
          saved.baseEditor.kitId || saved.baseEditor.selectionByRole.drums,
          profile.id);
        saved.baseEditor.kitId = baseKitId;
        baseInstrumentId = "";
        researchedPatch = factoryKitTemplatePatch(baseKitId);
        patch = kitBasePatch(baseKitId);
        selectedDrumVariantId = `factory:${baseKitId}:${drumPiece}`;
        drumKitCandidate = "custom";
      } else {
        saved.baseEditor.instrumentRoleId = role.id;
        const wanted = saved.baseEditor.instrumentId ||
          saved.baseEditor.selectionByRole[role.id];
        baseInstrumentId = instrumentChoiceExists(wanted)
          ? wanted : templateLibrary.instruments[0]?.id;
        selectedInstrumentVariantId = baseInstrumentId.startsWith("custom:")
          ? baseInstrumentId : "";
        baseKitId = "";
        researchedPatch = factoryInstrumentTemplatePatch(
          instrumentOriginTemplateId(baseInstrumentId));
        patch = instrumentBasePatch(baseInstrumentId);
        drumKitCandidate = "";
      }
    } else {
      saved.roleByProfile ||= {};
      saved.roleByProfile[profile.id] = role.id;
      researchedPatch = role.id === "drums"
        ? normalizeKit(role.parameters)
        : normalizePatch(role.parameters);
      patch = role.id === "drums"
        ? normalizeKit(state.patch || researchedPatch, researchedPatch)
        : normalizePatch(state.patch || researchedPatch);
      drumKitCandidate = role.id === "drums"
        ? (role.kitCandidates?.find(candidate => candidate.recommended)?.id ||
            role.kitCandidates?.[0]?.id || "")
        : "";
      treatmentId = templateLibrary?.treatments[state.treatmentId]
        ? state.treatmentId
        : (templateLibrary?.treatments[profile.styleId] ? profile.styleId : "neutral");
      baseInstrumentId = state.baseInstrumentId || "";
      baseKitId = state.baseKitId || "";
      if (state.mixSource === jam2MixSource) {
        patch = clone(researchedPatch);
        baseInstrumentId = "";
        baseKitId = "";
      } else if (role.id !== "drums" && instrumentChoiceExists(baseInstrumentId)) {
        patch = instrumentTemplatePatch(baseInstrumentId, role, profile.styleId);
      } else if (role.id === "drums" && baseKitId) {
        patch = kitTemplatePatch(baseKitId, role, profile.styleId);
        for (const [pieceId, choiceId] of Object.entries(
          state.drumPieceOptions || {})) {
          const custom = drumVariant(choiceId);
          if (custom) {
            patch.pieces[pieceId] = clone(custom.patch);
          } else if (choiceId.startsWith("factory:")) {
            const [, kitId] = choiceId.split(":");
            patch.pieces[pieceId] = clone(kitBasePatch(kitId).pieces[pieceId]);
          }
        }
      }
    }
    macroBase = clone(patch);
    resetMacroValues();
    rootSelect.value = String(role.id === "bass" ? 40 : role.id === "chords" ? 48 : 60);
    document.querySelector("#lab-design-name").textContent = isBaseMode()
      ? baseDisplayName()
      : `${role.designName} · ${role.targetPatchId}`;
    document.querySelector("#lab-notes").value =
      isBaseMode() ? "" : (state.notes || "");
    currentUrl = "";
    lastRenderDiagnostics = null;
    replayButton.disabled = true;
    const drumRole = role.id === "drums";
    jam2BackingButton.hidden = true;
    jam2DrumsButton.hidden = true;
    jam2ReferenceButton.hidden = true;
    setMixButtonsDisabled(false);
    renderStatus.textContent = labMode === "mix"
      ? "Choose the lineup, then play the designed style mix."
      : "No custom render yet.";
    configureAuditions();
    renderAllPatchControls();
    persist(false);
    if (isBaseMode()) {
      setBaseSaveStatus(true,
        `Editing ${baseDisplayName()}. Changes are drafts until explicitly saved as a named sound.`);
    }
  }

  function renderAllPatchControls() {
    const drums = role?.id === "drums";
    configureLabModeUi(drums);
    for (const section of document.querySelectorAll("[data-pitched-only]")) {
      section.hidden = labMode !== "instruments";
    }
    document.querySelector("#drum-kit-lab").hidden = labMode !== "drums";
    document.querySelector("#base-palette-lab").hidden =
      labMode !== "instruments";
    document.querySelector("#base-kit-lab").hidden =
      labMode !== "drums";
    renderBasePalette();
    if (labMode === "mix") {
      updateRawJson();
      return;
    }
    if (labMode === "drums") {
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

  function renderBasePalette() {
    if (!templateLibrary || !profile || !role) return;
    if (labMode === "mix") {
      renderCurrentLineup();
      renderStyleAssignment();
      return;
    }
    if (labMode === "drums") {
      renderBaseKits();
      return;
    }
    renderInstrumentChoices();
  }

  function configureLabModeUi(drums) {
    const instrumentMode = labMode === "instruments";
    const drumMode = labMode === "drums";
    const mixMode = labMode === "mix";
    document.querySelector("#lab-header-eyebrow").textContent = mixMode
      ? "Arrangement only" : "Sound creation";
    document.querySelector("#lab-header-title").textContent = instrumentMode
      ? "Base Instruments" : drumMode ? "Base Drums" : "Style Mixer";
    document.querySelector("#lab-header-description").textContent = instrumentMode
      ? "Create and save named instrument sounds. These sounds are available to every role in Style Mixer."
      : drumMode
        ? "Create named alternatives for individual pieces inside the Acoustic and Electronic kits."
        : "Choose which saved sounds play each role over the selected style pattern. Sound design controls stay on the two base pages.";
    document.querySelector("#lab-profile-field").hidden = false;
    document.querySelector("#lab-profile-label").textContent = mixMode
      ? "Style profile"
      : "Audition profile for generated pattern";
    document.querySelector("#lab-role-field").hidden = !instrumentMode;
    document.querySelector("#lab-role-label").textContent = "Audition register";
    for (const option of roleSelect.options) {
      option.hidden = instrumentMode && option.value === "drums";
    }
    document.querySelector("#lab-audition-field").hidden = mixMode;
    document.querySelector("#lab-root-field").hidden = !instrumentMode;
    document.querySelector("#lab-target-panel").hidden = true;
    document.querySelector("#lab-lineup-panel").hidden = !mixMode;
    document.querySelector("#lab-style-save-panel").hidden = !mixMode;
    document.querySelector("#style-assignment-lab").hidden = !mixMode;
    document.querySelector("#base-save-panel").hidden = mixMode;
    document.querySelector("#lab-snapshot-panel").hidden = true;
    document.querySelector("#lab-notes-panel").hidden = true;
    document.querySelector("#lab-raw-json").hidden = true;
    renderButton.hidden = mixMode;
    replayButton.hidden = mixMode;
    mixButton.hidden = !mixMode;
    document.querySelector("#lab-auto-render-label").hidden = mixMode;
    document.querySelector("#lab-drum-candidate-actions").hidden = true;
    document.querySelector("#lab-drum-pieces").hidden = true;
    document.querySelector("#lab-drum-piece-candidate-row").hidden = true;
    document.querySelector("#lab-drum-bus-group").hidden = true;
    document.querySelector("#lab-drum-kit-heading").textContent =
      `Edit ${drumPieceLabel(drumPiece)}`;
    document.querySelector("#lab-drum-kit-description").textContent =
      "Only this selected piece is being edited. Switch kit family, piece or saved option above when you want a different target.";
    document.querySelector("#lab-base-save-title").textContent = drumMode
      ? `Save ${drumPieceLabel(drumPiece)} option in ${baseDisplayName()}`
      : `Save an instrument based on ${baseDisplayName()}`;
    document.querySelector("#lab-update-base").hidden = drumMode
      ? !drumVariant(selectedDrumVariantId)
      : !instrumentVariant(selectedInstrumentVariantId);
  }

  function renderStyleAssignment() {
    treatmentId = profile.styleId;
    const baseSelect = document.querySelector("#lab-style-base");
    baseSelect.replaceChildren();
    if (role.id !== "drums") {
      baseSelect.add(new Option("Original researched sound", ""));
    }
    if (role.id !== "drums" && jam2ReferenceAvailable(role)) {
      baseSelect.add(new Option(
        "Current Jam2 native style sound",
        jam2StyleChoiceId));
    }
    const choices = role.id === "drums"
      ? templateLibrary.drumKits
      : [
          ...instrumentVariants().map(item => ({
            id: item.id,
            name: item.name,
            custom: true,
          })),
          ...templateLibrary.instruments,
        ];
    for (const choice of choices) {
      baseSelect.add(new Option(
        `${choice.name}${choice.custom ? " · my sound" : role.id === "drums" ? " · kit family" : " · factory"}`,
        choice.id));
    }
    const state = stateForCurrent();
    const currentId = state.mixSource === jam2MixSource
      ? jam2StyleChoiceId
      : role.id === "drums" ? baseKitId : baseInstrumentId;
    baseSelect.value = currentId === jam2StyleChoiceId ||
      choices.some(item => item.id === currentId)
      ? currentId : "";
    document.querySelector("#lab-arrangement-heading").textContent =
      role.id === "drums" ? "Drum kit" : `Sound for ${role.name}`;
    document.querySelector("#lab-arrangement-description").textContent =
      state.mixSource === jam2MixSource
        ? "Uses the exact current Jam2 reference stem for this role and profile; other roles remain independently selectable."
        : role.id === "drums"
        ? "Choose Acoustic or Electronic. The current style EQ and drum treatment are applied automatically."
        : "Choose any factory or saved Base Instrument sound. Instrument labels do not restrict which role can use it.";
    document.querySelector("#lab-style-drum-pieces").hidden = true;
  }

  function baseDisplayName() {
    if (role?.id === "drums") {
      return templateLibrary.drumKits.find(item => item.id === baseKitId)?.name ||
        "Base kit";
    }
    return instrumentChoiceName(baseInstrumentId);
  }

  function renderCurrentLineup() {
    const root = document.querySelector("#lab-current-lineup");
    if (!root) return;
    root.replaceChildren();
    for (const targetRole of profile.roles.filter(item => labRoles.has(item.id))) {
      const state = saved.patches?.[`${profile.id}/${targetRole.id}`] || {};
      let choice = targetRole.designName || "Researched default";
      if (state.mixSource === jam2MixSource) {
        choice = "Current Jam2 native";
      } else if (targetRole.id === "drums" && state.baseKitId) {
        choice = templateLibrary.drumKits.find(
          item => item.id === state.baseKitId)?.name || state.baseKitId;
      } else if (targetRole.id !== "drums" && state.baseInstrumentId) {
        choice = instrumentChoiceName(state.baseInstrumentId);
      } else if (state.patch) {
        choice = "Original researched sound";
      }
      const card = document.createElement("div");
      card.className = "lineup-role-card";
      const button = document.createElement("button");
      button.className = `lineup-role${targetRole.id === role.id ? " active" : ""}`;
      button.innerHTML = `<strong>${targetRole.name}</strong><span>${choice}</span>`;
      button.title = `${targetRole.name}: ${choice}`;
      button.addEventListener("click", () => loadRole(targetRole.id));
      const volume = document.createElement("label");
      volume.className = "lineup-volume";
      const trimDb = mixTrimDbForRole(targetRole.id);
      volume.innerHTML = `
        <span>Volume <output>${formatMixTrimDb(trimDb)}</output></span>
        <input type="range" min="-36" max="12" step="0.5" value="${trimDb}"
          aria-label="${targetRole.name} volume">
      `;
      const input = volume.querySelector("input");
      input.addEventListener("input", () => {
        const value = Number(input.value);
        saved.patches ||= {};
        const key = `${profile.id}/${targetRole.id}`;
        saved.patches[key] ||= {};
        saved.patches[key].mixTrimDb = value;
        volume.querySelector("output").textContent = formatMixTrimDb(value);
        persist(false);
        updatePlayingMixGain(targetRole.id);
      });
      card.append(button, volume);
      root.append(card);
    }
  }

  function updatePlayingMixGain(roleId) {
    if (!audioContext) return;
    for (const item of mixSources) {
      if (item.roleId !== roleId) continue;
      item.gain.gain.setTargetAtTime(
        mixGainForRole(roleId), audioContext.currentTime, .015);
    }
  }

  function renderBaseKits() {
    const kitSelect = document.querySelector("#lab-base-kit-choice");
    const pieceSelect = document.querySelector("#lab-base-drum-piece-choice");
    kitSelect.replaceChildren();
    pieceSelect.replaceChildren();
    for (const kit of templateLibrary.drumKits) {
      kitSelect.add(new Option(kit.name, kit.id));
    }
    for (const [id, name] of drumPieces) pieceSelect.add(new Option(name, id));
    kitSelect.value = baseKitId;
    pieceSelect.value = drumPiece;
    renderBaseDrumOptions();
  }

  function renderInstrumentChoices() {
    const select = document.querySelector("#lab-base-instrument-choice");
    select.replaceChildren();
    for (const variant of instrumentVariants()) {
      select.add(new Option(`${variant.name} · my sound`, variant.id));
    }
    for (const template of templateLibrary.instruments) {
      select.add(new Option(`${template.name} · ${template.family}`, template.id));
    }
    select.value = baseInstrumentId;
    const selected = instrumentVariant(baseInstrumentId);
    document.querySelector("#lab-base-name").value = selected?.name ||
      `${instrumentChoiceName(baseInstrumentId)} variation`;
    selectedInstrumentVariantId = selected?.id || "";
  }

  function drumPieceLabel(pieceId) {
    return drumPieces.find(([id]) => id === pieceId)?.[1] || pieceId;
  }

  function renderBaseDrumOptions() {
    const select = document.querySelector("#lab-base-drum-option");
    select.replaceChildren();
    const kitName = templateLibrary.drumKits.find(
      item => item.id === baseKitId)?.name || baseKitId;
    const factoryId = `factory:${baseKitId}:${drumPiece}`;
    select.add(new Option(`${kitName} ${drumPieceLabel(drumPiece)} · factory`, factoryId));
    for (const variant of drumVariants().filter(
      item => item.kitId === baseKitId && item.pieceId === drumPiece)) {
      select.add(new Option(`${variant.name} · my sound`, variant.id));
    }
    if (![...select.options].some(option => option.value === selectedDrumVariantId)) {
      selectedDrumVariantId = factoryId;
    }
    select.value = selectedDrumVariantId;
    const selected = drumVariant(selectedDrumVariantId);
    document.querySelector("#lab-base-name").value = selected?.name ||
      `${kitName} ${drumPieceLabel(drumPiece)} variation`;
  }

  function selectBaseDrumPiece(pieceId) {
    if (labMode !== "drums") return;
    drumPiece = drumPieces.some(([id]) => id === pieceId) ? pieceId : "kick";
    saved.baseEditor.drumPiece = drumPiece;
    patch = kitBasePatch(baseKitId);
    researchedPatch = factoryKitTemplatePatch(baseKitId);
    selectedDrumVariantId = `factory:${baseKitId}:${drumPiece}`;
    persist(false);
    renderAllPatchControls();
    setBaseSaveStatus(true,
      `Editing the factory ${baseDisplayName()} ${drumPieceLabel(drumPiece)}. Changes are an unsaved draft.`);
  }

  function loadDrumVariant(choiceId) {
    if (labMode !== "drums") return;
    const custom = drumVariant(choiceId);
    if (custom && custom.pieceId === drumPiece) {
      patch.pieces[drumPiece] = clone(custom.patch);
      selectedDrumVariantId = custom.id;
    } else {
      patch.pieces[drumPiece] = clone(
        kitBasePatch(baseKitId).pieces[drumPiece]);
      selectedDrumVariantId = `factory:${baseKitId}:${drumPiece}`;
    }
    persist(false);
    renderAllPatchControls();
    setBaseSaveStatus(true, custom
      ? `Loaded ${custom.name}. Edits remain drafts until Update or Save as new sound.`
      : `Loaded the factory ${baseDisplayName()} ${drumPieceLabel(drumPiece)}.`);
  }

  function renderStyleDrumPieces() {
    const root = document.querySelector("#lab-style-drum-pieces");
    root.replaceChildren();
    const state = stateForCurrent();
    state.drumPieceOptions ||= {};
    const selectedKitId = baseKitId || templateLibrary.drumKits[0].id;
    for (const [pieceId, pieceName] of drumPieces) {
      const label = document.createElement("label");
      label.innerHTML = `<span>${pieceName}</span><select></select>`;
      const select = label.querySelector("select");
      select.add(new Option("Use selected kit", ""));
      for (const kit of templateLibrary.drumKits) {
        select.add(new Option(`${kit.name} ${pieceName} · factory`,
          `factory:${kit.id}:${pieceId}`));
      }
      for (const variant of drumVariants().filter(
        item => item.pieceId === pieceId)) {
        select.add(new Option(`${variant.name} · my sound`, variant.id));
      }
      select.value = state.drumPieceOptions[pieceId] || "";
      select.addEventListener("change", event =>
        applyStyleDrumPiece(pieceId, event.target.value, selectedKitId));
      root.append(label);
    }
  }

  function applyStyleDrumPiece(pieceId, choiceId, selectedKitId) {
    if (labMode !== "mix" || role.id !== "drums") return;
    const state = stateForCurrent();
    state.drumPieceOptions ||= {};
    if (choiceId) state.drumPieceOptions[pieceId] = choiceId;
    else delete state.drumPieceOptions[pieceId];
    const custom = drumVariant(choiceId);
    if (custom) {
      patch.pieces[pieceId] = clone(custom.patch);
    } else if (choiceId.startsWith("factory:")) {
      const [, kitId] = choiceId.split(":");
      patch.pieces[pieceId] = clone(kitBasePatch(kitId).pieces[pieceId]);
    } else {
      patch.pieces[pieceId] = clone(
        kitTemplatePatch(selectedKitId, role, profile.styleId).pieces[pieceId]);
    }
    markDrumCustom();
    persist(false);
    renderCurrentLineup();
    document.querySelector("#lab-style-assignment-status").textContent =
      `${drumPieceLabel(pieceId)} assignment updated. The style pattern and saved base sounds were not changed.`;
  }

  function selectedTreatment(id = treatmentId) {
    return templateLibrary.treatments[id] ||
      templateLibrary.treatments.neutral;
  }

  function instrumentVariants() {
    saved.instrumentVariants ||= [];
    return saved.instrumentVariants;
  }

  function drumVariants() {
    saved.drumVariants ||= [];
    return saved.drumVariants;
  }

  function createVariantId(prefix) {
    return `${prefix}:${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
  }

  function instrumentVariant(choiceId) {
    return instrumentVariants().find(item => item.id === choiceId);
  }

  function instrumentChoiceExists(choiceId) {
    return Boolean(instrumentVariant(choiceId) ||
      templateLibrary.instruments.some(item => item.id === choiceId));
  }

  function instrumentOriginTemplateId(choiceId) {
    return instrumentVariant(choiceId)?.originTemplateId || choiceId;
  }

  function instrumentChoiceName(choiceId) {
    return instrumentVariant(choiceId)?.name ||
      templateLibrary.instruments.find(item => item.id === choiceId)?.name ||
      "Instrument sound";
  }

  function drumVariant(choiceId) {
    return drumVariants().find(item => item.id === choiceId);
  }

  function factoryInstrumentTemplatePatch(templateId) {
    const template = templateLibrary.instruments.find(item => item.id === templateId);
    if (!template) return null;
    return normalizePatch(deepMerge(
      templateLibrary.instrumentDefaults,
      template.patch || {}));
  }

  function instrumentBasePatch(templateId) {
    const custom = instrumentVariant(templateId);
    if (custom) return normalizePatch(custom.patch);
    const savedBase = saved.baseTemplates?.instruments?.[templateId];
    return savedBase
      ? normalizePatch(savedBase)
      : factoryInstrumentTemplatePatch(templateId);
  }

  function instrumentTemplatePatch(
      templateId,
      targetRole,
      appliedTreatmentId = treatmentId) {
    const template = templateLibrary.instruments.find(item => item.id === templateId);
    const custom = instrumentVariant(templateId);
    if (!template && !custom) return null;
    let values = instrumentBasePatch(templateId);
    const originTemplate = templateLibrary.instruments.find(
      item => item.id === (custom?.originTemplateId || templateId));
    if (!originTemplate?.styleReady) {
      const treatment = selectedTreatment(appliedTreatmentId);
      values = deepMerge(values, treatment.common || {});
      values = deepMerge(values, treatment.roles?.[targetRole.id] || {});
      if (appliedTreatmentId === profile.styleId) {
        values = deepMerge(
          values,
          templateLibrary.profileTreatments[profile.id]?.instruments?.[targetRole.id] || {});
      }
    }
    return normalizePatch(values);
  }

  function factoryKitTemplatePatch(templateId) {
    const template = templateLibrary.drumKits.find(item => item.id === templateId);
    if (!template) return null;
    const pieces = {};
    for (const [id] of drumPieces) {
      pieces[id] = deepMerge(
        templateLibrary.drumPieceDefaults,
        template.pieces?.[id] || {});
    }
    let values = {
      candidateId: `base-${template.id}`,
      candidateName: template.name,
      recommended: false,
      description: template.description,
      researchFamily: template.researchFamily || `shared-base-${template.id}`,
      sourceReferences: template.sourceReferences || [
        "DaisySP synthesis primitives",
        "Workbench shared base palette v1",
      ],
      bus: clone(template.bus),
      pieces,
    };
    return normalizeKit(values, values);
  }

  function kitBasePatch(templateId) {
    const savedBase = saved.baseTemplates?.kits?.[templateId];
    return savedBase
      ? normalizeKit(savedBase, savedBase)
      : factoryKitTemplatePatch(templateId);
  }

  function applyStyleRideDamping(values, styleId) {
    if (!values?.pieces?.ride || !["jazz", "modal-jam"].includes(styleId)) {
      return values;
    }
    const ride = values.pieces.ride;
    ride.level = Math.min(Number(ride.level ?? .34),
      styleId === "jazz" ? .27 : .25);
    ride.decay = Math.min(Number(ride.decay ?? .9), .72);
    ride.onsetSofteningSeconds = Math.max(
      Number(ride.onsetSofteningSeconds || 0), .022);
    ride.colourStage ||= {};
    ride.colourStage.reconstructionLowpassHz = Math.min(
      Number(ride.colourStage.reconstructionLowpassHz || 20000), 6500);
    ride.transient ||= {};
    ride.transient.level = Number(ride.transient.level || 0) * .55;
    ride.transient.tone = Number(ride.transient.tone ?? .5) * .75;
    ride.texture ||= {};
    ride.texture.level = Number(ride.texture.level || 0) * .75;
    ride.texture.tone = Number(ride.texture.tone ?? .5) * .8;
    ride.texture.decaySeconds = Math.min(
      Number(ride.texture.decaySeconds || 0), 1.45);
    for (const band of ride.modalBands || []) {
      band.decaySeconds = Number(band.decaySeconds || 0) * .82;
      if (Number(band.frequencyHz || 0) >= 4000) {
        band.level = Number(band.level || 0) * .58;
      }
      band.accentGain = Math.min(
        Number(band.accentGain ?? 1), Number(band.normalGain ?? 1) * 1.05);
    }
    for (const band of ride.noiseBands || []) {
      if (Number(band.frequencyHz || 0) >= 4000) {
        band.level = Number(band.level || 0) * .68;
      }
      band.accentGain = Math.min(Number(band.accentGain ?? 1), 1.1);
    }
    return values;
  }

  function kitTemplatePatch(
      templateId,
      targetRole,
      appliedTreatmentId = treatmentId) {
    const template = templateLibrary.drumKits.find(item => item.id === templateId);
    if (!template) return null;
    let values = kitBasePatch(templateId);
    const treatment = selectedTreatment(appliedTreatmentId);
    values.candidateId = `base-${template.id}-${appliedTreatmentId}`;
    values.candidateName = `${template.name} / ${treatment.name}`;
    const styleTreatment = templateLibrary.drumTreatments[appliedTreatmentId] || {};
    values = deepMerge(values, {
      bus: styleTreatment.bus || {},
      pieces: styleTreatment.pieces || {},
    });
    const kitSpecific = styleTreatment.kits?.[templateId] || {};
    values = deepMerge(values, {
      bus: kitSpecific.bus || {},
      pieces: kitSpecific.pieces || {},
    });
    if (appliedTreatmentId === profile.styleId) {
      const specific = templateLibrary.profileTreatments[profile.id]?.drums || {};
      values = deepMerge(values, {
        bus: specific.bus || {},
        pieces: specific.pieces || {},
      });
    }
    return normalizeKit(
      applyStyleRideDamping(values, appliedTreatmentId), values);
  }

  function applyInstrumentTemplate(templateId) {
    const next = labMode === "instruments"
      ? instrumentBasePatch(templateId)
      : instrumentTemplatePatch(templateId, role, profile.styleId);
    if (!next) return;
    patch = next;
    baseInstrumentId = templateId;
    if (labMode === "instruments") {
      saved.baseEditor.instrumentId = templateId;
      selectedInstrumentVariantId = templateId.startsWith("custom:")
        ? templateId : "";
      researchedPatch = factoryInstrumentTemplatePatch(
        instrumentOriginTemplateId(templateId));
    } else {
      const state = stateForCurrent();
      state.mixSource = "";
      state.baseInstrumentId = templateId;
      state.treatmentId = profile.styleId;
    }
    rebaseMacros();
    persist(false);
    renderAllPatchControls();
    const template = templateLibrary.instruments.find(item => item.id === templateId);
    document.querySelector("#lab-base-palette-status").textContent =
      labMode === "instruments"
        ? `Editing ${instrumentChoiceName(templateId)}. Changes are a draft until saved below.`
        : "";
    if (labMode === "instruments") {
      document.querySelector("#lab-base-name").value =
        instrumentVariant(templateId)?.name || `${template?.name || "Instrument"} variation`;
      setBaseSaveStatus(true,
        `Loaded ${instrumentChoiceName(templateId)}. Edit freely, then save as a new sound or update the selected saved sound.`);
    }
    scheduleRender();
  }

  function applyKitTemplate(templateId) {
    const next = labMode === "drums"
      ? kitBasePatch(templateId)
      : kitTemplatePatch(templateId, role, profile.styleId);
    if (!next) return;
    patch = next;
    baseKitId = templateId;
    drumKitCandidate = "custom";
    if (labMode === "drums") {
      saved.baseEditor.kitId = templateId;
      selectedDrumVariantId = `factory:${templateId}:${drumPiece}`;
      researchedPatch = factoryKitTemplatePatch(templateId);
    } else {
      const state = stateForCurrent();
      state.mixSource = "";
      state.baseKitId = templateId;
      state.treatmentId = profile.styleId;
      state.drumPieceOptions = {};
    }
    persist(false);
    renderAllPatchControls();
    const template = templateLibrary.drumKits.find(item => item.id === templateId);
    document.querySelector("#lab-base-kit-status").textContent =
      labMode === "drums"
        ? `Editing ${template.name} / ${drumPieceLabel(drumPiece)}. Save the piece as a named option below.`
        : "";
    if (labMode === "drums") {
      document.querySelector("#lab-base-name").value =
        `${template.name} ${drumPieceLabel(drumPiece)} variation`;
      setBaseSaveStatus(true,
        `Loaded the ${template.name} starting point for ${drumPieceLabel(drumPiece)}.`);
    }
    scheduleRender();
  }

  function applySelectedStyleBase() {
    if (labMode !== "mix") return;
    const templateId = document.querySelector("#lab-style-base").value;
    treatmentId = profile.styleId;
    if (templateId === jam2StyleChoiceId) {
      const state = stateForCurrent();
      state.mixSource = jam2MixSource;
      state.patch = clone(researchedPatch);
      state.baseInstrumentId = "";
      state.baseKitId = "";
      state.drumPieceOptions = {};
      baseInstrumentId = "";
      baseKitId = "";
      patch = clone(researchedPatch);
      persist(false);
      renderAllPatchControls();
      document.querySelector("#lab-style-assignment-status").textContent =
        `Current Jam2 native is assigned to ${role.name}. Other roles are unchanged.`;
      return;
    }
    if (!templateId) {
      patch = clone(researchedPatch);
      const state = stateForCurrent();
      state.mixSource = "";
      state.patch = clone(patch);
      state.baseInstrumentId = "";
      state.baseKitId = "";
      state.drumPieceOptions = {};
      baseInstrumentId = "";
      baseKitId = "";
      persist(false);
      renderAllPatchControls();
      document.querySelector("#lab-style-assignment-status").textContent =
        `Restored ${role.name}'s original researched sound. The pattern did not change.`;
      return;
    }
    if (role.id === "drums") {
      applyKitTemplate(templateId);
    } else {
      applyInstrumentTemplate(templateId);
    }
    document.querySelector("#lab-style-assignment-status").textContent =
      `${baseDisplayName()} is assigned to ${role.name}. ${profile.name}'s pattern did not change.`;
  }

  function saveCurrentBase() {
    if (!isBaseMode()) return;
    const requestedName = document.querySelector("#lab-base-name").value.trim();
    if (labMode === "drums") {
      const name = requestedName || `${baseDisplayName()} ${drumPieceLabel(drumPiece)}`;
      const variant = {
        id: createVariantId("drum"),
        name,
        kitId: baseKitId,
        pieceId: drumPiece,
        patch: clone(patch.pieces[drumPiece]),
      };
      drumVariants().push(variant);
      selectedDrumVariantId = variant.id;
    } else {
      const name = requestedName || `${baseDisplayName()} variation`;
      const variant = {
        id: createVariantId("custom"),
        name,
        originTemplateId: instrumentOriginTemplateId(baseInstrumentId),
        patch: clone(patch),
      };
      instrumentVariants().push(variant);
      baseInstrumentId = variant.id;
      selectedInstrumentVariantId = variant.id;
      saved.baseEditor.instrumentId = variant.id;
    }
    localStorage.setItem(storageKey, JSON.stringify(saved));
    setBaseSaveStatus(true,
      `Saved ${requestedName || baseDisplayName()} as a new browser-library sound. Existing arrangements were not changed.`);
    renderBasePalette();
    renderAllPatchControls();
    updateRawJson();
  }

  function updateCurrentBase() {
    if (!isBaseMode()) return;
    const name = document.querySelector("#lab-base-name").value.trim();
    if (labMode === "drums") {
      const variant = drumVariant(selectedDrumVariantId);
      if (!variant) return;
      variant.name = name || variant.name;
      variant.patch = clone(patch.pieces[drumPiece]);
    } else {
      const variant = instrumentVariant(selectedInstrumentVariantId);
      if (!variant) return;
      variant.name = name || variant.name;
      variant.patch = clone(patch);
    }
    localStorage.setItem(storageKey, JSON.stringify(saved));
    renderAllPatchControls();
    setBaseSaveStatus(true, "Updated the selected browser-library sound. Style Mixer arrangements that reference it will use the updated sound when loaded or played.");
  }

  function loadSavedBase() {
    if (labMode === "drums") loadDrumVariant(selectedDrumVariantId);
    else if (labMode === "instruments") applyInstrumentTemplate(baseInstrumentId);
  }

  function loadFactoryBase() {
    if (!isBaseMode()) return;
    if (labMode === "drums") {
      const factory = factoryKitTemplatePatch(baseKitId);
      patch.pieces[drumPiece] = clone(factory.pieces[drumPiece]);
      selectedDrumVariantId = `factory:${baseKitId}:${drumPiece}`;
    } else {
      patch = factoryInstrumentTemplatePatch(
        instrumentOriginTemplateId(baseInstrumentId));
    }
    rebaseMacros();
    renderAllPatchControls();
    setBaseSaveStatus(false,
      "Loaded the factory starting point as an unsaved draft.");
    updateRawJson();
  }

  function setBaseSaveStatus(savedClean, message = "") {
    const status = document.querySelector("#lab-base-save-status");
    if (!status) return;
    status.textContent = message || (savedClean
      ? `${baseDisplayName()} matches its saved browser copy.`
      : `Unsaved edits to ${baseDisplayName()}.`);
    status.classList.toggle("unsaved", !savedClean);
  }

  function applyRecommendedPalette() {
    if (!templateLibrary || !profile) return;
    const treatment = selectedTreatment();
    for (const targetRole of profile.roles.filter(item => labRoles.has(item.id))) {
      const key = `${profile.id}/${targetRole.id}`;
      saved.patches ||= {};
      saved.patches[key] ||= {};
      saved.patches[key].mixSource = "";
      if (targetRole.id === "drums") {
        const kitId = templateLibrary.drumTreatments[treatmentId]?.kitId ||
          treatment.palette?.drums || "acoustic";
        saved.patches[key].patch = kitTemplatePatch(kitId, targetRole);
        saved.patches[key].baseKitId = kitId;
      } else {
        const templateId = treatment.palette?.[targetRole.id];
        const next = instrumentTemplatePatch(templateId, targetRole);
        if (!next) continue;
        saved.patches[key].patch = next;
        saved.patches[key].baseInstrumentId = templateId;
      }
      saved.patches[key].treatmentId = treatmentId;
    }
    localStorage.setItem(storageKey, JSON.stringify(saved));
    const status = document.querySelector("#lab-style-assignment-status");
    if (status) {
      status.textContent = `Applied the ${treatment.name} shared palette to every available role. Use Designed style mix to hear it together.`;
    }
    loadRole(role.id);
    scheduleRender();
  }

  function deepMerge(base, override) {
    const result = clone(base || {});
    for (const [key, value] of Object.entries(override || {})) {
      if (value && typeof value === "object" && !Array.isArray(value)) {
        result[key] = deepMerge(result[key] || {}, value);
      } else {
        result[key] = value;
      }
    }
    return result;
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
    const positive = value => Math.max(0, value);
    const suppress = (value, amount, exponent = 2) => amount < 0
      ? value * Math.pow(1 + amount, exponent)
      : value;

    const hardness = macroValues.hardness;
    patch.attackSeconds *= Math.pow(2, -6 * hardness);
    patch.decaySeconds *= Math.pow(2, -1.4 * hardness);
    patch.transientMix = suppress(patch.transientMix, hardness) +
      positive(hardness) * .76;
    patch.transientSeconds *= Math.pow(2, -2.6 * hardness);
    patch.filterEnvelopeHz += hardness * 6800;
    patch.stringDamping += hardness * .46;

    const brightness = macroValues.brightness;
    patch.filterCutoffHz *= Math.pow(2, 4.5 * brightness);
    patch.resonance += positive(brightness) * .3;
    patch.shape += .58 * brightness;
    patch.width += .28 * brightness;
    patch.stringBrightness += .78 * brightness;
    patch.spectralShape += .72 * brightness;
    patch.fmIndex = brightness < 0
      ? patch.fmIndex * Math.pow(2, 3.5 * brightness)
      : patch.fmIndex + brightness * 10;
    patch.fmRatio *= Math.pow(2, 1.5 * brightness);
    patch.harmonicFamily = Math.round(
      patch.harmonicFamily + brightness * 4);
    patch.formantRatio *= Math.pow(2, 2 * brightness);
    patch.formantRatio2 *= Math.pow(2, 1.6 * brightness);
    if (patch.fixedFormantHz > 0) {
      patch.fixedFormantHz *= Math.pow(2, 2.5 * brightness);
    }
    if (patch.fixedFormant2Hz > 0) {
      patch.fixedFormant2Hz *= Math.pow(2, 2.2 * brightness);
    }

    const body = macroValues.body;
    patch.subMix = suppress(patch.subMix, body) + positive(body) * .82;
    patch.oscillator2Mix = suppress(patch.oscillator2Mix, body) +
      positive(body) * .58;
    patch.stringDouble = suppress(patch.stringDouble, body) +
      positive(body) * .84;
    patch.detuneCents = body < 0
      ? patch.detuneCents * Math.pow(1 + body, 2)
      : patch.detuneCents + body * 15;
    patch.filterCutoffHz *= Math.pow(2, -1.2 * body);
    patch.cabinet += positive(body) * .4;

    const grit = macroValues.grit;
    patch.voiceDrive *= Math.pow(2, 3.4 * grit);
    patch.busDrive *= Math.pow(2, 2.8 * grit);
    patch.filterDrive *= Math.pow(2, 2.6 * grit);
    patch.wavefold = suppress(patch.wavefold, grit) + positive(grit) * 6.5;
    patch.noiseMix = suppress(patch.noiseMix, grit) + positive(grit) * .48;
    patch.cabinet = suppress(patch.cabinet, grit) + positive(grit) * .72;
    patch.fmIndex += positive(grit) * 5;
    patch.resonance += positive(grit) * .16;

    const movement = macroValues.movement;
    patch.glideSeconds = suppress(patch.glideSeconds, movement) +
      positive(movement) * .62;
    patch.vibratoCents = suppress(patch.vibratoCents, movement, 3) +
      positive(movement) * 58;
    patch.vibratoRateHz += positive(movement) * 3.8;
    patch.vibratoDelaySeconds = movement > 0
      ? patch.vibratoDelaySeconds * (1 - movement)
      : patch.vibratoDelaySeconds;
    patch.tremoloDepth = suppress(patch.tremoloDepth, movement, 3) +
      positive(movement) * .74;
    patch.tremoloRateHz += positive(movement) * 7;
    patch.chorusMix = suppress(patch.chorusMix, movement, 3) +
      positive(movement) * .58;
    patch.chorusDepth = suppress(patch.chorusDepth, movement, 3) +
      positive(movement) * .75;
    patch.chorusRateHz += positive(movement) * 1.2;
    patch.spectralMode += positive(movement) * .72;

    const length = macroValues.length;
    patch.decaySeconds *= Math.pow(2, 4 * length);
    patch.releaseSeconds *= Math.pow(2, 5 * length);
    patch.sustain += .68 * length;
    patch.stringDamping -= .56 * length;
    patch.transientSeconds *= Math.pow(2, 1.6 * length);
    patch.delaySeconds *= Math.pow(2, 1.4 * length);
    patch.reverbSeconds *= Math.pow(2, 2.2 * length);

    const space = macroValues.space;
    patch.delayMix = suppress(patch.delayMix, space, 3) +
      positive(space) * .68;
    patch.delaySeconds *= Math.pow(2, 1.7 * space);
    patch.chorusMix = suppress(patch.chorusMix, space, 3) +
      positive(space) * .52;
    patch.chorusDepth = suppress(patch.chorusDepth, space, 3) +
      positive(space) * .82;
    patch.chorusRateHz *= Math.pow(2, -.8 * space);
    patch.reverbMix = suppress(patch.reverbMix, space, 3) +
      positive(space) * .68;
    patch.reverbSeconds *= Math.pow(2, 1.8 * space);
    patch.stereoWidth += positive(space) * .72;
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
          ? ["glideSeconds", "vibratoCents", "vibratoRateHz", "tremoloDepth", "tremoloRateHz", "chorusMix", "chorusDepth"]
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
    const state = saved.patches?.[key] || {};
    let active = state.patch;
    if (candidateRole.id === "drums" && state.baseKitId) {
      active = kitTemplatePatch(state.baseKitId, candidateRole, profile.styleId);
      for (const [pieceId, choiceId] of Object.entries(
        state.drumPieceOptions || {})) {
        const custom = drumVariant(choiceId);
        if (custom) {
          active.pieces[pieceId] = clone(custom.patch);
        } else if (choiceId.startsWith("factory:")) {
          const [, kitId] = choiceId.split(":");
          active.pieces[pieceId] = clone(kitBasePatch(kitId).pieces[pieceId]);
        }
      }
    } else if (candidateRole.id !== "drums" &&
               instrumentChoiceExists(state.baseInstrumentId)) {
      active = instrumentTemplatePatch(
        state.baseInstrumentId, candidateRole, profile.styleId);
    }
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

  function drumLayerState(piece) {
    const sourceLayerGain = Number(piece.sourceLayerGain ?? 1);
    return {
      sourceA: piece.source !== "off" && sourceLayerGain > 0,
      sourceB: piece.secondSource !== "off" && Number(piece.blend || 0) > 0,
      transient: piece.transient?.type !== "off" &&
        Number(piece.transient?.level || 0) > 0,
      texture: piece.texture?.type !== "off" &&
        Number(piece.texture?.level || 0) > 0,
      synthCharacter: piece.synthLayer?.source !== "off" &&
        Number(piece.synthLayer?.level || 0) > 0,
      modalBands: (piece.modalBands || []).filter(
        band => Number(band.level || 0) > 0).length,
      noiseBands: (piece.noiseBands || []).filter(
        band => Number(band.level || 0) > 0).length,
    };
  }

  function pitchedLayerState(active) {
    return {
      sourceA: active.source !== "off",
      sourceB: active.secondSource !== "off" &&
        Number(active.sourceBlend || 0) > 0,
      noise: Number(active.noiseMix || 0) > 0,
      transient: Number(active.transientMix || 0) > 0,
      chorus: Number(active.chorusMix || 0) > 0,
      delay: Number(active.delayMix || 0) > 0,
      reverb: Number(active.reverbMix || 0) > 0,
    };
  }

  function drumPieceSelectionRecord(state, pieceId, piece) {
    const choiceId = state.drumPieceOptions?.[pieceId] || "";
    const layers = drumLayerState(piece);
    const enabled = Number(piece.level || 0) > 0 && Boolean(
      layers.sourceA || layers.sourceB || layers.transient || layers.texture ||
      layers.synthCharacter || layers.modalBands || layers.noiseBands);
    const custom = drumVariant(choiceId);
    if (custom) {
      return {choiceId, enabled, source: "saved-base-drum", name: custom.name};
    }
    if (choiceId.startsWith("factory:")) {
      const [, kitId] = choiceId.split(":");
      const kitName = templateLibrary.drumKits.find(
        item => item.id === kitId)?.name || kitId;
      return {
        choiceId,
        enabled,
        source: "factory-piece",
        kitId,
        name: `${kitName} ${drumPieceLabel(pieceId)}`,
      };
    }
    const kitName = templateLibrary.drumKits.find(
      item => item.id === state.baseKitId)?.name ||
      state.baseKitId || "Original researched kit";
    return {
      choiceId: "",
      enabled,
      source: "selected-kit",
      kitId: state.baseKitId || null,
      name: `${kitName} ${drumPieceLabel(pieceId)}`,
    };
  }

  function completeStyleRoleRecord(candidateRole) {
    const key = `${profile.id}/${candidateRole.id}`;
    const state = saved.patches?.[key] || {};
    if (state.mixSource === jam2MixSource) {
      const candidate = candidateRole.candidates?.find(item => item.id === "jam2");
      return {
        role: candidateRole.id,
        roleName: candidateRole.name,
        enabled: true,
        source: "jam2-native",
        parameterType: "jam2-native",
        selection: {
          candidateId: candidate?.id || "jam2",
          name: candidate?.name || "Current Jam2 native",
          model: candidate?.model || null,
          referencePath: candidate?.path || null,
        },
        parameters: null,
      };
    }
    const request = activeMixRequest(candidateRole);
    if (candidateRole.id === "drums") {
      const kit = clone(request.kit);
      return {
        role: candidateRole.id,
        roleName: candidateRole.name,
        enabled: true,
        source: "designed",
        parameterType: "drum-kit",
        selection: {
          baseKitId: state.baseKitId || null,
          baseKitName: templateLibrary.drumKits.find(
            item => item.id === state.baseKitId)?.name || null,
          treatmentId: state.treatmentId || profile.styleId,
          pieces: Object.fromEntries(drumPieces.map(([pieceId]) => [
            pieceId, drumPieceSelectionRecord(state, pieceId, kit.pieces[pieceId]),
          ])),
        },
        enabledLayers: Object.fromEntries(drumPieces.map(([pieceId]) => [
          pieceId, drumLayerState(kit.pieces[pieceId]),
        ])),
        parameters: kit,
      };
    }
    const active = clone(request.patch);
    return {
      role: candidateRole.id,
      roleName: candidateRole.name,
      enabled: true,
      source: "designed",
      parameterType: "instrument-patch",
      selection: {
        baseInstrumentId: state.baseInstrumentId || null,
        baseInstrumentName: state.baseInstrumentId
          ? instrumentChoiceName(state.baseInstrumentId) : null,
        treatmentId: state.treatmentId || profile.styleId,
      },
      enabledLayers: pitchedLayerState(active),
      parameters: active,
    };
  }

  function completeStyleMixRecord() {
    persist(false);
    const roles = profile.roles
      .filter(candidateRole => labRoles.has(candidateRole.id))
      .map(completeStyleRoleRecord);
    return {
      schema: "jam2-style-mix-handoff-v1",
      librarySchema: templateLibrary?.schema || null,
      profileId: profile.id,
      profileName: profile.name,
      styleId: profile.styleId,
      soundBrief: profile.soundBrief,
      performance: {
        bpm: profile.bpm,
        meter: profile.meter,
        bars: profile.bars,
        patternSource: "current-profile-generated-performance",
      },
      mix: {
        masterGain: .42,
        roles: Object.fromEntries(roles.map(item => [item.role, {
          enabled: item.enabled,
          gain: mixGainForRole(item.role),
          baseGain: defaultMixGainForRole(item.role),
          trimDb: mixTrimDbForRole(item.role),
        }])),
      },
      roles,
      updatedAt: new Date().toISOString(),
    };
  }

  async function saveCompleteStyleMix() {
    const status = document.querySelector("#lab-save-style-mix-status");
    status.classList.remove("error");
    status.textContent = "Saving complete resolved style mixâ€¦";
    try {
      const record = completeStyleMixRecord();
      const response = await fetch("/api/style-mix", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(record),
      });
      const result = await response.json();
      if (!response.ok || !result.ok) {
        if (response.status === 404 && result.error === "Unknown endpoint.") {
          throw new Error(
            "The running workbench server is an older version. Close its server window and reopen open-workbench.cmd, then save again.");
        }
        throw new Error(result.error || "Style mix save failed.");
      }
      status.textContent =
        `Saved exact ${profile.name} snapshot to ${result.absolutePath || result.path}.`;
    } catch (error) {
      status.textContent = error.message;
      status.classList.add("error");
    }
  }

  async function playInMix() {
    if (!profile || !role) return;
    stopAll();
    renderStatus.classList.remove("error");
    setMixButtonsDisabled(true);
    renderStatus.textContent = "Loading sample-aligned style mix…";
    try {
      persist();
      const nativeRoleCount = profile.roles.filter(candidateRole =>
        saved.patches?.[`${profile.id}/${candidateRole.id}`]?.mixSource ===
          jam2MixSource).length;
      renderStatus.textContent =
        `Preparing ${profile.roles.length - nativeRoleCount} rendered and ${nativeRoleCount} current Jam2 native roles…`;
      const entries = await Promise.all(profile.roles.map(async candidateRole => {
        const state = saved.patches?.[
          `${profile.id}/${candidateRole.id}`] || {};
        if (state.mixSource === jam2MixSource) {
          return jam2Entry(candidateRole);
        }
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
        "Playing the selected sample-aligned lineup; Jam2 native roles use their exact current reference stems.";
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

  function jam2ReferenceAvailable(candidateRole) {
    return Boolean(candidateRole?.candidates?.some(
      candidate => candidate.id === "jam2" && candidate.path));
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
      const roleId = entries[index].role.id;
      gain.gain.value = mixGainForRole(roleId);
      source.buffer = buffer;
      source.connect(gain).connect(master);
      source.start(start);
      return {source, gain, roleId};
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
    for (const item of mixSources) {
      try { item.source.stop(); } catch {}
    }
    mixSources = [];
  }

  function scheduleRender() {
    persist(false);
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
              ? "Saved complete kit"
              : `Saved complete patch · ${sourceLabel(state.snapshots[slot].source)}`)
          : "Empty · loads style start"}</span>
        <div class="snapshot-actions">
          <button data-save>Save whole ${role.id === "drums" ? "kit" : "patch"}</button>
          <button data-load>${state.snapshots[slot] ? "Load saved" : "Load style start"}</button>
          ${state.snapshots[slot] ? "<button data-clear>Clear slot</button>" : ""}
        </div>
      `;
      wrapper.querySelector("[data-save]").addEventListener("click", () => {
        state.snapshots[slot] = clone(patch);
        persist(false);
        renderSnapshots();
      });
      wrapper.querySelector("[data-load]").addEventListener("click", () => {
        patch = clone(state.snapshots[slot] || researchedPatch);
        rebaseMacros();
        persist(false);
        renderAllPatchControls();
        scheduleRender();
      });
      wrapper.querySelector("[data-clear]")?.addEventListener("click", () => {
        delete state.snapshots[slot];
        persist(false);
        renderSnapshots();
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

  function persist(markBaseDirty = true) {
    if (profile && role && patch && labMode === "mix") {
      stateForCurrent().patch = clone(patch);
    }
    if (isBaseMode() && markBaseDirty) setBaseSaveStatus(false);
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
      stereoWidth: 0,
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
        sourceLayerGain: clamp(
          candidate.sourceLayerGain ?? defaults.sourceLayerGain ?? 1,
          0,
          2),
        onsetSofteningSeconds: clamp(
          candidate.onsetSofteningSeconds ??
            defaults.onsetSofteningSeconds ?? 0,
          0,
          .1),
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
        modalBands: normalizeDrumDetailBands(
          candidate.modalBands,
          defaults.modalBands,
          "modal"),
        noiseBands: normalizeDrumDetailBands(
          candidate.noiseBands,
          defaults.noiseBands,
          "noise"),
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

  function normalizeDrumDetailBands(source, defaults, kind) {
    const maximum = kind === "modal" ? 12 : 4;
    const bands = Array.isArray(source)
      ? source : (Array.isArray(defaults) ? defaults : []);
    return bands.slice(0, maximum).map(item => {
      const band = item && typeof item === "object" ? item : {};
      const next = {
        frequencyHz: clamp(
          band.frequencyHz ?? (kind === "modal" ? 1000 : 4000),
          kind === "modal" ? 20 : 40,
          20000),
        level: clamp(band.level ?? 0, 0, 1),
        decaySeconds: clamp(band.decaySeconds ?? .2, .005, 8),
        attackSeconds: clamp(band.attackSeconds ?? .001, .0001, .25),
        delaySeconds: clamp(band.delaySeconds ?? 0, 0, .25),
        highpassHz: clamp(band.highpassHz ?? 0, 0, 20000),
        velocityCurve: clamp(band.velocityCurve ?? 1, .2, 3),
        ghostGain: clamp(band.ghostGain ?? .55, 0, 3),
        normalGain: clamp(band.normalGain ?? 1, 0, 3),
        accentGain: clamp(band.accentGain ?? 1.25, 0, 3),
        roomSend: clamp(band.roomSend ?? 1, 0, 2),
      };
      if (kind === "modal") {
        next.detuneCents = clamp(band.detuneCents ?? 0, -50, 50);
        next.phaseCycles = clamp(band.phaseCycles ?? -1, -1, 1);
      } else {
        next.q = clamp(band.q ?? 1, .1, 30);
      }
      return next;
    });
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
      auditionSelect.add(new Option("Arpeggiated chord sequence", "arpeggio"));
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
    for (const base of templateLibrary?.drumKits || []) {
      pieceSelect.add(new Option(
        `Base ${base.name} / ${drumPieces.find(([id]) => id === drumPiece)?.[1]}`,
        `base:${base.id}`));
    }
    for (const candidate of role.kitCandidates || []) {
      pieceSelect.add(new Option(
        `${candidate.recommended ? "Feedback focus - " : ""}${candidate.name} / ${drumPieces.find(([id]) => id === drumPiece)?.[1]}`,
        candidate.id));
    }
    pieceSelect.add(new Option("Custom", "custom"));
    const matchingBase = (templateLibrary?.drumKits || []).find(base => {
      const candidateKit = kitTemplatePatch(base.id, role);
      return JSON.stringify(candidateKit.pieces[drumPiece]) ===
        JSON.stringify(piece);
    });
    const matchingPiece = (role.kitCandidates || []).find(candidate => {
      const candidateKit = normalizeKit(
        candidate.parameters, role.parameters);
      return JSON.stringify(candidateKit.pieces[drumPiece]) ===
        JSON.stringify(piece);
    });
    pieceSelect.value = matchingBase
      ? `base:${matchingBase.id}` : (matchingPiece?.id || "custom");
    const describedBase = (templateLibrary?.drumKits || []).find(
      candidate => `base:${candidate.id}` === pieceSelect.value);
    const described = (role.kitCandidates || []).find(
      candidate => candidate.id ===
        (drumKitCandidate === "custom"
          ? pieceSelect.value : drumKitCandidate));
    const candidateDescription =
      describedBase?.description || described?.description || patch.description ||
      "Manual component combination.";
    const detailSummary =
      `${piece.modalBands.length} modal / ${piece.noiseBands.length} noise bands`;
    document.querySelector("#lab-drum-candidate-description").textContent =
      `${piece.intendedIdentity || "Unspecified piece identity"} — ${candidateDescription} — ${detailSummary}`;
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
    if (isBaseMode()) {
      const drumMode = labMode === "drums";
      const savedInBrowser = drumMode
        ? Boolean(drumVariant(selectedDrumVariantId))
        : Boolean(instrumentVariant(baseInstrumentId));
      return {
        schema: "jam2-base-sound-patch-v1",
        kind: drumMode ? "drum-piece" : "instrument",
        baseId: drumMode ? selectedDrumVariantId : baseInstrumentId,
        baseName: baseDisplayName(),
        kitId: drumMode ? baseKitId : null,
        pieceId: drumMode ? drumPiece : null,
        savedInBrowser,
        storageBoundary: "Draft until explicitly saved or updated in the browser sound library.",
        updatedAt: new Date().toISOString(),
        patch: clone(drumMode ? patch.pieces[drumPiece] : patch),
      };
    }
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
      baseTemplate: {
        librarySchema: templateLibrary?.schema || null,
        instrumentId: role.id === "drums" ? null : (baseInstrumentId || null),
        kitId: role.id === "drums" ? (baseKitId || null) : null,
        treatmentId,
      },
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
    const defaults = {
      delaySeconds: .23,
      wavefold: 0,
      velocitySensitivity: 1,
      pitchEnvelopeSemitones: 0,
      pitchEnvelopeSeconds: .04,
      pitchAttackGain: 0,
      pitchAttackSeconds: .04,
      filterEnvelopeDecaySeconds: 0,
      filterEnvelopeSustain: .025,
      filterKeyTracking: .72,
      filterVelocitySensitivity: 1,
      highpassCutoffHz: 0,
      reverbMix: 0,
      reverbSeconds: 1.8,
      reverbDamping: .55,
      reverbPreDelaySeconds: 0,
      stereoSpread: 0,
      stereoWidth: 1,
    };
    for (const parameter of parameters) {
      const fallback = defaults[parameter.key] ?? parameter.min;
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
