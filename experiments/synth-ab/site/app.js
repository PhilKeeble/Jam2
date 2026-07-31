(() => {
  const manifest = window.JAM2_SOUND_DESIGN_MANIFEST;
  const status = document.querySelector("#status");
  const workbench = document.querySelector("#workbench");
  if (!manifest) return;
  if (window.location.protocol === "file:") {
    status.classList.add("error");
    status.innerHTML = `
      <strong>This page needs its local audio server.</strong>
      Browsers silence <code>file://</code> media routed through Web Audio.
      Close this tab and run <code>open-workbench.cmd</code> from the
      <code>experiments/synth-ab</code> folder.
    `;
    return;
  }

  const storageKey = "jam2-sound-design-workbench-v2";
  const legacyStorageKey = "jam2-sound-design-workbench-v1";
  const profiles = new Map(manifest.profiles.map(profile => [profile.id, profile]));
  const styles = new Map(manifest.styles.map(style => [style.id, style]));
  let saved = loadSaved();
  let currentStyleId = validStyle(saved.currentStyleId) ? saved.currentStyleId : manifest.styles[0].id;
  let currentProfileId = validProfileForStyle(saved.currentProfileId, currentStyleId)
    ? saved.currentProfileId
    : styles.get(currentStyleId).profileIds[0];
  let context = null;
  let masterOutput = null;
  let tracks = new Map();
  let scenePlaying = false;

  status.remove();
  workbench.hidden = false;
  renderStyleNav();
  renderProfile();

  document.querySelector("#play-scene").addEventListener("click", playScene);
  document.querySelector("#stop-scene").addEventListener("click", stopScene);
  document.querySelector("#global-engine").addEventListener("change", async event => {
    const candidateId = event.target.value;
    if (!["jam2", "daisy", "hybrid"].includes(candidateId)) return;
    const wasPlaying = scenePlaying;
    stopScene();
    const profile = profiles.get(currentProfileId);
    for (const role of profile.roles) {
      ensureRoleState(role);
      profileState().roles[role.id].candidate = candidateId;
    }
    save();
    renderProfile();
    if (wasPlaying) await playScene();
  });
  document.querySelector("#loop-scene").addEventListener("change", event => {
    for (const track of tracks.values()) track.audio.loop = event.target.checked;
  });
  document.querySelector("#profile-notes").addEventListener("input", event => {
    profileState().notes = event.target.value;
    save();
    flashSaved();
  });
  document.querySelector("#reset-profile").addEventListener("click", () => {
    stopScene();
    delete saved.profiles[currentProfileId];
    save();
    renderProfile();
  });

  function renderStyleNav() {
    const nav = document.querySelector("#style-nav");
    nav.replaceChildren();
    for (const style of manifest.styles) {
      const button = document.createElement("button");
      button.className = "style-button";
      button.classList.toggle("active", style.id === currentStyleId);
      button.innerHTML = `<span>${style.name}</span><small>${style.profileIds.length} ${style.profileIds.length === 1 ? "profile" : "profiles"}</small>`;
      button.addEventListener("click", () => {
        if (style.id === currentStyleId) return;
        stopScene();
        currentStyleId = style.id;
        currentProfileId = style.profileIds.includes(saved.currentProfileId)
          ? saved.currentProfileId
          : style.profileIds[0];
        saved.currentStyleId = currentStyleId;
        saved.currentProfileId = currentProfileId;
        save();
        renderStyleNav();
        renderProfile();
      });
      nav.append(button);
    }
  }

  function renderProfile() {
    disposeTracks();
    const profile = profiles.get(currentProfileId);
    const style = styles.get(currentStyleId);
    const state = profileState();

    document.querySelector("#style-label").textContent = style.name;
    document.querySelector("#profile-title").textContent = profile.name;
    document.querySelector("#profile-meta").textContent =
      `${profile.bpm} BPM · ${profile.meter} · ${profile.bars}-bar fixed audition · seed ${profile.seed}` +
      (profile.experimental ? " · experimental" : "");
    document.querySelector("#profile-meta").title =
      "Transparent scene master gain: 0.42 (-7.5 dB) for summing headroom.";
    document.querySelector("#sound-brief").textContent = profile.soundBrief;
    document.querySelector("#sound-limits").textContent = profile.limitations;
    document.querySelector("#profile-notes").value = state.notes || "";
    renderProfileTabs(style);

    const roles = document.querySelector("#roles");
    roles.replaceChildren();
    for (const role of profile.roles) {
      ensureRoleState(role);
      roles.append(renderRole(profile, role));
      createTrack(role);
    }
    syncGlobalEngine(profile);
  }

  function syncGlobalEngine(profile) {
    const choices = new Set(
      profile.roles.map(role => profileState().roles[role.id].candidate)
    );
    document.querySelector("#global-engine").value =
      choices.size === 1 ? [...choices][0] : "mixed";
  }

  function renderProfileTabs(style) {
    const tabs = document.querySelector("#profile-tabs");
    tabs.replaceChildren();
    for (const profileId of style.profileIds) {
      const profile = profiles.get(profileId);
      if (!profile) continue;
      const button = document.createElement("button");
      button.className = "profile-tab";
      button.classList.toggle("active", profileId === currentProfileId);
      button.textContent = profile.name;
      button.addEventListener("click", () => {
        if (profileId === currentProfileId) return;
        stopScene();
        currentProfileId = profileId;
        saved.currentProfileId = currentProfileId;
        save();
        renderProfile();
      });
      tabs.append(button);
    }
  }

  function renderRole(profile, role) {
    const state = profileState().roles[role.id];
    const card = document.createElement("article");
    card.className = "role-card";
    card.dataset.role = role.id;
    card.innerHTML = `
      <div class="role-heading">
        <div>
          <p class="eyebrow">${role.name}</p>
          <h3>${role.designName}</h3>
          <p>${role.auditionPolicy}</p>
        </div>
        <button class="solo-button" data-action="solo">▶ Audition selected role</button>
      </div>
      <div class="candidate-grid">
        ${role.candidates.map(candidate => `
          <section class="candidate ${state.candidate === candidate.id ? "selected" : ""}" data-candidate="${candidate.id}">
            <div class="candidate-topline">
              <span class="engine-badge ${candidate.engine.toLowerCase().replaceAll(" ", "-")}">${candidate.engine}</span>
              ${state.candidate === candidate.id ? `<span class="chosen">Chosen</span>` : ""}
            </div>
            <h4>${candidate.name}</h4>
            <p class="model">${candidate.model}</p>
            <p>${candidate.rationale}</p>
            <div class="candidate-actions">
              <button data-action="preview" data-candidate="${candidate.id}">▶ Preview</button>
              <button data-action="choose" data-candidate="${candidate.id}" class="${state.candidate === candidate.id ? "primary" : ""}">${state.candidate === candidate.id ? "Selected" : "Use this"}</button>
            </div>
          </section>
        `).join("")}
      </div>
      <div class="controls">
        ${control("level", "Level", state.level, "Balance this role in the selected scene.")}
        ${control("tone", "Tone", state.tone, "A live low-pass direction test; source synthesis remains unchanged.")}
        ${control("drive", "Drive", state.drive, "Live waveshaping after the source candidate.")}
        ${control("space", "Space", state.space, "Live tempo-neutral delay send for directional audition.")}
      </div>
      <details class="technical">
        <summary>Raw candidate design data</summary>
        <pre>${escapeHtml(JSON.stringify(role.parameters, null, 2))}</pre>
      </details>
    `;

    card.querySelector('[data-action="solo"]').addEventListener("click", () => soloRole(role.id));
    for (const button of card.querySelectorAll('[data-action="preview"]')) {
      button.addEventListener("click", () => previewCandidate(role.id, button.dataset.candidate));
    }
    for (const button of card.querySelectorAll('[data-action="choose"]')) {
      button.addEventListener("click", () => {
        profileState().roles[role.id].candidate = button.dataset.candidate;
        save();
        updateTrackSource(role.id);
        renderProfile();
      });
    }
    for (const input of card.querySelectorAll('input[type="range"]')) {
      input.addEventListener("input", () => {
        const roleState = profileState().roles[role.id];
        roleState[input.dataset.control] = Number(input.value);
        input.closest(".control").querySelector("output").value = input.value;
        applyTrackControls(role.id);
        save();
        flashSaved();
      });
    }
    return card;
  }

  function control(id, label, value, description) {
    return `
      <label class="control">
        <span><strong>${label}</strong><output>${value}</output></span>
        <input type="range" min="0" max="100" value="${value}" data-control="${id}">
        <small>${description}</small>
      </label>
    `;
  }

  function ensureAudioContext() {
    if (context) return context;
    const AudioContextType = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextType) throw new Error("This browser does not support Web Audio.");
    context = new AudioContextType();
    masterOutput = context.createGain();
    masterOutput.gain.value = 0.42;
    masterOutput.connect(context.destination);
    return context;
  }

  function createTrack(role) {
    const audioContext = ensureAudioContext();
    const audio = new Audio();
    audio.preload = "auto";
    audio.loop = document.querySelector("#loop-scene").checked;
    const source = audioContext.createMediaElementSource(audio);
    const filter = audioContext.createBiquadFilter();
    filter.type = "lowpass";
    filter.Q.value = 0.55;
    const shaper = audioContext.createWaveShaper();
    shaper.oversample = "2x";
    const gain = audioContext.createGain();
    const delay = audioContext.createDelay(1.5);
    delay.delayTime.value = 0.27;
    const feedback = audioContext.createGain();
    feedback.gain.value = 0.20;
    const wet = audioContext.createGain();

    source.connect(filter);
    filter.connect(shaper);
    shaper.connect(gain);
    gain.connect(masterOutput);
    shaper.connect(delay);
    delay.connect(feedback);
    feedback.connect(delay);
    delay.connect(wet);
    wet.connect(masterOutput);
    tracks.set(role.id, {role, audio, source, filter, shaper, gain, delay, feedback, wet});
    updateTrackSource(role.id);
    applyTrackControls(role.id);
  }

  function updateTrackSource(roleId, temporaryCandidateId = null) {
    const track = tracks.get(roleId);
    if (!track) return;
    const state = profileState().roles[roleId];
    const candidateId = temporaryCandidateId || state.candidate;
    const candidate = track.role.candidates.find(value => value.id === candidateId);
    if (!candidate) return;
    const wasPlaying = !track.audio.paused;
    track.audio.pause();
    track.audio.src = candidate.path;
    track.audio.load();
    if (wasPlaying && scenePlaying) track.audio.play().catch(() => {});
  }

  function applyTrackControls(roleId) {
    const track = tracks.get(roleId);
    if (!track) return;
    const state = profileState().roles[roleId];
    const now = context.currentTime;
    const level = Math.pow(state.level / 100, 1.35) * 1.05;
    const cutoff = 350 * Math.pow(48, state.tone / 100);
    const wet = Math.pow(state.space / 100, 1.4) * 0.48;
    track.gain.gain.setTargetAtTime(level, now, 0.015);
    track.filter.frequency.setTargetAtTime(cutoff, now, 0.015);
    track.wet.gain.setTargetAtTime(wet, now, 0.015);
    track.delay.delayTime.setTargetAtTime(0.16 + 0.32 * state.space / 100, now, 0.02);
    track.shaper.curve = driveCurve(state.drive);
  }

  function driveCurve(amount) {
    const samples = 1024;
    const curve = new Float32Array(samples);
    const strength = 1 + 11 * amount / 100;
    const normal = Math.tanh(strength);
    for (let index = 0; index < samples; index++) {
      const x = index * 2 / (samples - 1) - 1;
      curve[index] = amount === 0 ? x : Math.tanh(strength * x) / normal;
    }
    return curve;
  }

  async function playScene() {
    stopScene();
    scenePlaying = true;
    const audioContext = ensureAudioContext();
    if (audioContext.state === "suspended") await audioContext.resume();
    for (const [roleId, track] of tracks) {
      updateTrackSource(roleId);
      track.audio.currentTime = 0;
      track.audio.loop = document.querySelector("#loop-scene").checked;
    }
    await Promise.allSettled([...tracks.values()].map(track => track.audio.play()));
    document.querySelector("#play-scene").textContent = "↻ Restart selected scene";
  }

  function stopScene() {
    scenePlaying = false;
    for (const track of tracks.values()) {
      track.audio.pause();
      try { track.audio.currentTime = 0; } catch {}
    }
    const button = document.querySelector("#play-scene");
    if (button) button.textContent = "▶ Play selected scene";
  }

  async function soloRole(roleId) {
    stopScene();
    const audioContext = ensureAudioContext();
    if (audioContext.state === "suspended") await audioContext.resume();
    const track = tracks.get(roleId);
    updateTrackSource(roleId);
    track.audio.currentTime = 0;
    track.audio.loop = false;
    track.audio.play().catch(() => {});
  }

  async function previewCandidate(roleId, candidateId) {
    stopScene();
    const audioContext = ensureAudioContext();
    if (audioContext.state === "suspended") await audioContext.resume();
    const track = tracks.get(roleId);
    updateTrackSource(roleId, candidateId);
    track.audio.currentTime = 0;
    track.audio.loop = false;
    track.audio.play().catch(() => {});
    track.audio.addEventListener("ended", () => updateTrackSource(roleId), {once: true});
  }

  function disposeTracks() {
    for (const track of tracks.values()) {
      track.audio.pause();
      track.source.disconnect();
      track.filter.disconnect();
      track.shaper.disconnect();
      track.gain.disconnect();
      track.delay.disconnect();
      track.feedback.disconnect();
      track.wet.disconnect();
    }
    tracks = new Map();
  }

  function profileState() {
    saved.profiles ||= {};
    saved.profiles[currentProfileId] ||= {notes: "", roles: {}};
    return saved.profiles[currentProfileId];
  }

  function ensureRoleState(role) {
    const state = profileState();
    state.roles ||= {};
    state.roles[role.id] ||= {...role.defaults};
    const roleState = state.roles[role.id];
    const candidateIds = role.candidates.map(candidate => candidate.id);
    if (!candidateIds.includes(roleState.candidate)) roleState.candidate = role.defaults.candidate;
    for (const key of ["level", "tone", "drive", "space"]) {
      if (!Number.isFinite(roleState[key])) roleState[key] = role.defaults[key];
    }
  }

  function validStyle(id) {
    return typeof id === "string" && styles.has(id);
  }

  function validProfileForStyle(id, styleId) {
    return typeof id === "string" && styles.get(styleId)?.profileIds.includes(id) && profiles.has(id);
  }

  function loadSaved() {
    try {
      const current = JSON.parse(localStorage.getItem(storageKey));
      if (current) return current;
      const legacy = JSON.parse(localStorage.getItem(legacyStorageKey));
      if (!legacy) return {profiles: {}};
      for (const profile of Object.values(legacy.profiles || {})) {
        for (const role of Object.values(profile.roles || {})) {
          role.tone = 100;
          role.drive = 0;
          role.space = 0;
        }
      }
      localStorage.setItem(storageKey, JSON.stringify(legacy));
      return legacy;
    } catch {
      return {profiles: {}};
    }
  }

  function save() {
    saved.currentStyleId = currentStyleId;
    saved.currentProfileId = currentProfileId;
    localStorage.setItem(storageKey, JSON.stringify(saved));
  }

  function flashSaved() {
    const label = document.querySelector("#save-status");
    label.textContent = "Saved just now";
    clearTimeout(flashSaved.timer);
    flashSaved.timer = setTimeout(() => label.textContent = "Saved locally", 1200);
  }

  function escapeHtml(text) {
    return text
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;");
  }
})();
