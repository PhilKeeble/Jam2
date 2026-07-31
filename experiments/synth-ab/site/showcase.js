(() => {
  const manifest = window.DAISY_SHOWCASE_MANIFEST;
  const status = document.querySelector("#status");
  if (!manifest) return;
  status.remove();

  const diagnostics = document.querySelector("#diagnostics");
  for (const group of manifest.diagnostics) {
    const card = document.createElement("article");
    card.className = "idea showcase-card";
    card.innerHTML = `
      <div class="idea-heading">
        <div>
          <p class="eyebrow">DaisySP building blocks</p>
          <h2>${group.title}</h2>
        </div>
        <p class="meta">${manifest.sampleRate / 1000} kHz · stereo WAV</p>
      </div>
      <p class="summary">${group.summary}</p>
      <div class="clips showcase-clips">${group.clips.map(renderClip).join("")}</div>
    `;
    diagnostics.appendChild(card);
  }

  const voices = document.querySelector("#voices");
  for (const voice of manifest.voices) {
    const card = document.createElement("article");
    card.className = "idea voice-card";
    card.innerHTML = `
      <div class="idea-heading">
        <div>
          <p class="eyebrow">Compact voice design</p>
          <h2>${voice.title}</h2>
        </div>
      </div>
      <p class="summary">${voice.explanation}</p>
      <div class="listen-for"><strong>Listen for</strong><span>${voice.listenFor}</span></div>
      <div class="clips paired-clips">${voice.clips.map(renderClip).join("")}</div>
    `;
    voices.appendChild(card);
  }

  function renderClip(clip) {
    return `
      <div class="clip">
        <div>
          <strong>${clip.title}</strong>
          <span>${clip.model}</span>
        </div>
        <audio controls preload="metadata" src="${clip.path}"></audio>
        <p class="clip-note">${clip.listenFor}</p>
      </div>
    `;
  }
})();
