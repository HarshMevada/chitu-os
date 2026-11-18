"""Train Chitu's own wake word with microWakeWord (runs in GitHub Actions).

Steps (each is skipped if its output already exists):
  1. Synthetic voice samples of the wake word (+ similar-sounding "do not wake" phrases)
  2. Background noise / room echo / music data for augmentation
  3. Spectrogram features
  4. Ready-made negative features (speech, parties, silence) from the microWakeWord authors
  5. Training (CPU) + test, then the model and its settings are copied to OUT_DIR

Usage: python train.py <config.yaml> <work_dir> <out_dir>
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys

import yaml

CFG_PATH, WORK, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = yaml.safe_load(open(CFG_PATH, encoding="utf-8"))
os.makedirs(WORK, exist_ok=True)
os.makedirs(OUT, exist_ok=True)
os.chdir(WORK)

GEN_MODEL = "models/en_US-libritts_r-medium.pt"
HINDI_VOICES = ["hi_IN-pratham-medium", "hi_IN-priyamvada-medium", "hi_IN-rohan-medium"]


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def generate(phrases, count, out_dir, models, batch_size=25):
    """Generate `count` samples per phrase into out_dir with unique names."""
    os.makedirs(out_dir, exist_ok=True)
    for i, text in enumerate(phrases):
        tmp = f"_gen_{os.path.basename(out_dir)}_{i}"
        if os.path.isdir(tmp):
            shutil.rmtree(tmp)
        cmd = [sys.executable, "-m", "piper_sample_generator", text, "--max-samples", str(count),
               "--output-dir", tmp]
        for m in models:
            cmd += ["--model", m]
        if models[0].endswith(".pt"):
            cmd += ["--batch-size", str(batch_size), "--max-speakers", "800"]
        run(cmd)
        for j, f in enumerate(sorted(glob.glob(f"{tmp}/*.wav"))):
            shutil.move(f, os.path.join(out_dir, f"p{i}_{j}.wav"))
        shutil.rmtree(tmp)


# ---------------------------------------------------------------- 1. samples
if not os.path.isdir("positive"):
    generate(cfg["positive_phrases"], cfg["positive_samples_per_phrase"], "positive_tmp", [GEN_MODEL])
    # Hindi voices: Indian pronunciation of the same name
    hindi = [f"voices/{v}.onnx" for v in HINDI_VOICES if os.path.exists(f"voices/{v}.onnx")]
    if hindi and cfg.get("positive_phrases_hindi"):
        try:
            generate(cfg["positive_phrases_hindi"], cfg["hindi_samples_per_phrase"], "positive_hi", hindi)
            for f in glob.glob("positive_hi/*.wav"):
                shutil.move(f, os.path.join("positive_tmp", "hi_" + os.path.basename(f)))
        except subprocess.CalledProcessError as e:
            print("Hindi samples skipped:", e)
    os.rename("positive_tmp", "positive")
print("positive samples:", len(glob.glob("positive/*.wav")))

if not os.path.isdir("adversarial"):
    generate(cfg["negative_phrases"], cfg["negative_samples_per_phrase"], "adversarial_tmp", [GEN_MODEL])
    os.rename("adversarial_tmp", "adversarial")
print("adversarial samples:", len(glob.glob("adversarial/*.wav")))

# ---------------------------------------------------------------- 2. augmentation data
import numpy as np  # noqa: E402
import scipy.io.wavfile  # noqa: E402
import datasets  # noqa: E402
from pathlib import Path  # noqa: E402

if not os.path.isdir("mit_rirs"):
    os.makedirs("mit_rirs_tmp")
    rir = datasets.load_dataset("davidscripka/MIT_environmental_impulse_responses", split="train", streaming=True)
    for row in rir:
        name = row["audio"]["path"].split("/")[-1]
        scipy.io.wavfile.write(os.path.join("mit_rirs_tmp", name), 16000,
                               (row["audio"]["array"] * 32767).astype(np.int16))
    os.rename("mit_rirs_tmp", "mit_rirs")


def to_16k_wavs(src_glob, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    files = [str(p) for p in Path(".").glob(src_glob)]
    ds = datasets.Dataset.from_dict({"audio": files}).cast_column("audio", datasets.Audio(sampling_rate=16000))
    for row in ds:
        try:
            name = os.path.splitext(row["audio"]["path"].split("/")[-1])[0] + ".wav"
            scipy.io.wavfile.write(os.path.join(out_dir, name), 16000,
                                   (row["audio"]["array"] * 32767).astype(np.int16))
        except Exception as e:  # a few files may be broken
            print("skip", e)


if not os.path.isdir("audioset_16k"):
    os.makedirs("audioset", exist_ok=True)
    run(["wget", "-q", "-O", "audioset/bal_train09.tar",
         "https://huggingface.co/datasets/agkphysics/AudioSet/resolve/main/data/bal_train09.tar"])
    run(["tar", "-xf", "audioset/bal_train09.tar", "-C", "audioset"])
    to_16k_wavs("audioset/audio/**/*.flac", "audioset_16k_tmp")
    shutil.rmtree("audioset")
    os.rename("audioset_16k_tmp", "audioset_16k")

if not os.path.isdir("fma_16k"):
    os.makedirs("fma", exist_ok=True)
    run(["wget", "-q", "-O", "fma/fma_xs.zip",
         "https://huggingface.co/datasets/mchl914/fma_xsmall/resolve/main/fma_xs.zip"])
    run(["unzip", "-q", "fma/fma_xs.zip", "-d", "fma"])
    to_16k_wavs("fma/fma_small/**/*.mp3", "fma_16k_tmp")
    shutil.rmtree("fma")
    os.rename("fma_16k_tmp", "fma_16k")

# ---------------------------------------------------------------- 3. features
from mmap_ninja.ragged import RaggedMmap  # noqa: E402
from microwakeword.audio.augmentation import Augmentation  # noqa: E402
from microwakeword.audio.clips import Clips  # noqa: E402
from microwakeword.audio.spectrograms import SpectrogramGeneration  # noqa: E402

augmenter = Augmentation(
    augmentation_duration_s=3.2,
    augmentation_probabilities={
        "SevenBandParametricEQ": 0.1,
        "TanhDistortion": 0.1,
        "PitchShift": 0.1,
        "BandStopFilter": 0.1,
        "AddColorNoise": 0.1,
        "AddBackgroundNoise": 0.75,
        "Gain": 1.0,
        "RIR": 0.5,
    },
    impulse_paths=["mit_rirs"],
    background_paths=["fma_16k", "audioset_16k"],
    background_min_snr_db=-5,
    background_max_snr_db=10,
    min_jitter_s=0.195,
    max_jitter_s=0.205,
)


def make_features(clip_dir, out_dir, train_repeat):
    if os.path.isdir(out_dir):
        return
    clips = Clips(input_directory=clip_dir, file_pattern="*.wav", max_clip_duration_s=None,
                  remove_silence=False, random_split_seed=10, split_count=0.1)
    tmp = out_dir + "_tmp"
    for split, split_name, repeat, slide in [("training", "train", train_repeat, 10),
                                             ("validation", "validation", 1, 10),
                                             ("testing", "test", 1, 1)]:
        os.makedirs(os.path.join(tmp, split), exist_ok=True)
        spec = SpectrogramGeneration(clips=clips, augmenter=augmenter, slide_frames=slide, step_ms=10)
        RaggedMmap.from_generator(out_dir=os.path.join(tmp, split, "wakeword_mmap"),
                                  sample_generator=spec.spectrogram_generator(split=split_name, repeat=repeat),
                                  batch_size=100, verbose=True)
    os.rename(tmp, out_dir)


make_features("positive", "features_positive", 2)
make_features("adversarial", "features_adversarial", 1)

# ---------------------------------------------------------------- 4. negative datasets
if not os.path.isdir("negative_datasets"):
    os.makedirs("negative_tmp")
    root = "https://huggingface.co/datasets/kahrendt/microwakeword/resolve/main/"
    for fname in ["dinner_party.zip", "dinner_party_eval.zip", "no_speech.zip", "speech.zip"]:
        run(["wget", "-q", "-O", f"negative_tmp/{fname}", root + fname])
        run(["unzip", "-q", f"negative_tmp/{fname}", "-d", "negative_tmp"])
        os.remove(f"negative_tmp/{fname}")
    os.rename("negative_tmp", "negative_datasets")

# ---------------------------------------------------------------- 5. training
steps = int(cfg.get("training_steps", 10000))
train_cfg = {
    "window_step_ms": 10,
    "train_dir": "trained_models/wakeword",
    "features": [
        {"features_dir": "features_positive", "sampling_weight": 2.0, "penalty_weight": 1.0, "truth": True,
         "truncation_strategy": "truncate_start", "type": "mmap"},
        {"features_dir": "features_adversarial", "sampling_weight": 3.0, "penalty_weight": 1.5, "truth": False,
         "truncation_strategy": "truncate_start", "type": "mmap"},
        {"features_dir": "negative_datasets/speech", "sampling_weight": 10.0, "penalty_weight": 1.0,
         "truth": False, "truncation_strategy": "random", "type": "mmap"},
        {"features_dir": "negative_datasets/dinner_party", "sampling_weight": 10.0, "penalty_weight": 1.0,
         "truth": False, "truncation_strategy": "random", "type": "mmap"},
        {"features_dir": "negative_datasets/no_speech", "sampling_weight": 5.0, "penalty_weight": 1.0,
         "truth": False, "truncation_strategy": "random", "type": "mmap"},
        {"features_dir": "negative_datasets/dinner_party_eval", "sampling_weight": 0.0, "penalty_weight": 1.0,
         "truth": False, "truncation_strategy": "split", "type": "mmap"},
    ],
    "training_steps": [steps],
    "positive_class_weight": [1],
    "negative_class_weight": [20],
    "learning_rates": [0.001],
    "batch_size": 128,
    "time_mask_max_size": [0],
    "time_mask_count": [0],
    "freq_mask_max_size": [0],
    "freq_mask_count": [0],
    "eval_step_interval": 500,
    "clip_duration_ms": 1500,
    "target_minimization": 0.9,
    "minimization_metric": None,
    "maximization_metric": "average_viable_recall",
}
yaml.dump(train_cfg, open("training_parameters.yaml", "w"))

run([sys.executable, "-m", "microwakeword.model_train_eval",
     "--training_config=training_parameters.yaml",
     "--train", "1", "--restore_checkpoint", "1",
     "--test_tf_nonstreaming", "0", "--test_tflite_nonstreaming", "0",
     "--test_tflite_nonstreaming_quantized", "0", "--test_tflite_streaming", "0",
     "--test_tflite_streaming_quantized", "1",
     "--use_weights", "best_weights",
     "mixednet",
     "--pointwise_filters", "64,64,64,64",
     "--repeat_in_block", "1, 1, 1, 1",
     "--mixconv_kernel_sizes", "[5], [7,11], [9,15], [23]",
     "--residual_connection", "0,0,0,0",
     "--first_conv_filters", "32",
     "--first_conv_kernel_size", "5",
     "--stride", "3"])

# ---------------------------------------------------------------- 6. pick a cutoff, export
model_dir = "trained_models/wakeword/tflite_stream_state_internal_quant"
roc = open(os.path.join(model_dir, "tflite_streaming_roc.txt")).read()
print(roc)
points = [(float(c), float(frr), float(faph)) for c, frr, faph in
          re.findall(r"Cutoff ([\d.]+): frr=([\d.]+); faph=([\d.]+)", roc)]
# Lowest cutoff (= easiest to trigger) that wakes up falsely at most ~0.5 times per hour
good = sorted([p for p in points if p[2] <= 0.5], key=lambda p: p[0])
cutoff, frr, faph = good[0] if good else max(points, key=lambda p: p[0])
cutoff = min(max(cutoff, 0.5), 0.99)

name = cfg["name"]
shutil.copy(os.path.join(model_dir, "stream_state_internal_quant.tflite"), os.path.join(OUT, f"{name}.tflite"))
manifest = {
    "type": "micro",
    "wake_word": cfg["wake_word"],
    "author": "Chitu OS (trained with microWakeWord)",
    "trained_languages": ["en", "hi"],
    "version": 2,
    "micro": {
        "probability_cutoff": round(cutoff, 2),
        "sliding_window_size": 5,
        "feature_step_size": 10,
        "tensor_arena_size": 30000,
        "minimum_esphome_version": "2024.7.0",
    },
    "test_results": {"false_rejection_rate": frr, "false_accepts_per_hour": faph, "steps": steps},
}
json.dump(manifest, open(os.path.join(OUT, f"{name}.json"), "w"), indent=2)
print(json.dumps(manifest, indent=2))
