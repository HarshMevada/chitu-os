# "Chitu" wake word (microWakeWord)

Trains Chitu's own wake word on GitHub's computers (free), with synthetic voices
(Piper TTS) instead of recordings.

* Edit `config.yaml` (phrase spellings, sample counts, training steps) and push:
  the **Train wake word** workflow runs (2-4 hours) and commits the result to
  `main/assets/wakeword/chitu.tflite` + `chitu.json`.
* The firmware build then offers "Chitu (trained)" on the install page.

Data note: the background-noise and negative datasets used here are for
personal, non-commercial use only.
