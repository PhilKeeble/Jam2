# Local tuner

Jam2's tuner analyses only the current user's selected, downmixed input. It
does not analyse peers, prepared tracks, metronome audio, or the output mix,
and it does not place pitch data on the network.

The tuner is off at application startup. Enabling it starts a dedicated
worker and activates a separate lock-free input ring; disabling it removes
the callback tap and stops the worker. The audio callback never invokes aubio
and does not allocate, lock, log, throw, or block.

The worker uses an 8,192-sample YINFFT window and a 1,024-sample hop. For
fundamentals below 100 Hz, a bounded raw-sample period refinement around
aubio's candidate removes low-bass windowing bias. Both stages and all visual
smoothing remain off the audio callback.

The compact performance card intentionally exposes only the detected note,
the moving nebula orb, the central tuning target, the gold in-tune state, and
the power control. Clicking the card opens the same presentation on a wider
rail without navigating away from the performance page.

Raw frequency, cents, confidence, analysis timing, rejected-window count, and
tuner-ring depth/overruns remain available in renderer diagnostics and stats
CSV output.
