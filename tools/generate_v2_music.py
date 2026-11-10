#!/usr/bin/env python3
"""Generate original mono MP3 cues for the V2.1 robot dog.

Requires ``numpy`` and ``lameenc``.  The synthesis is deterministic and uses
no sampled or third-party music, so the checked-in assets are reproducible.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import lameenc
import numpy as np


SAMPLE_RATE = 16000
RNG = np.random.default_rng(20260910)
POWER_ON_STYLE = "cheerful-major-160bpm"
POWER_ON_CANDIDATE_STYLES = (
    "01_tennis_marimba",
    "02_sunshine_bells",
    "03_arcade_victory",
    "04_stadium_bounce",
    "05_robot_spark",
)


def envelope(length: int, attack: float, release: float) -> np.ndarray:
    env = np.ones(length, dtype=np.float64)
    attack_n = min(length, max(1, int(attack * SAMPLE_RATE)))
    release_n = min(length, max(1, int(release * SAMPLE_RATE)))
    env[:attack_n] = np.linspace(0.0, 1.0, attack_n, endpoint=False)
    env[-release_n:] *= np.linspace(1.0, 0.0, release_n, endpoint=True)
    return env


def add_tone(track: np.ndarray, start: float, duration: float, frequency: float,
             gain: float, waveform: str = "sine", attack: float = 0.008,
             release: float = 0.08) -> None:
    begin = int(start * SAMPLE_RATE)
    count = min(len(track) - begin, int(duration * SAMPLE_RATE))
    if begin < 0 or count <= 0:
        return
    t = np.arange(count, dtype=np.float64) / SAMPLE_RATE
    phase = 2.0 * np.pi * frequency * t
    if waveform == "square":
        wave = np.tanh(3.0 * np.sin(phase))
    elif waveform == "saw":
        wave = 2.0 * ((frequency * t) % 1.0) - 1.0
    else:
        wave = np.sin(phase)
    track[begin:begin + count] += gain * wave * envelope(count, attack, release)


def add_noise_hit(track: np.ndarray, start: float, duration: float, gain: float,
                  decay: float = 18.0) -> None:
    begin = int(start * SAMPLE_RATE)
    count = min(len(track) - begin, int(duration * SAMPLE_RATE))
    if begin < 0 or count <= 0:
        return
    t = np.arange(count, dtype=np.float64) / SAMPLE_RATE
    noise = RNG.standard_normal(count)
    # A one-pole difference removes low-frequency rumble and makes a compact hit.
    bright = np.empty_like(noise)
    bright[0] = noise[0]
    bright[1:] = noise[1:] - 0.82 * noise[:-1]
    track[begin:begin + count] += gain * bright * np.exp(-decay * t)


def add_kick(track: np.ndarray, start: float, gain: float = 0.32) -> None:
    begin = int(start * SAMPLE_RATE)
    count = min(len(track) - begin, int(0.16 * SAMPLE_RATE))
    if begin < 0 or count <= 0:
        return
    t = np.arange(count, dtype=np.float64) / SAMPLE_RATE
    phase = 2.0 * np.pi * (78.0 * t - 120.0 * t * t)
    track[begin:begin + count] += gain * np.sin(phase) * np.exp(-24.0 * t)


def finish(track: np.ndarray) -> np.ndarray:
    # Gentle saturation keeps simultaneous percussion and chords from clipping.
    track = np.tanh(track * 1.15)
    peak = float(np.max(np.abs(track)))
    if peak > 0.0:
        track *= 0.88 / peak
    fade = int(0.04 * SAMPLE_RATE)
    track[:fade] *= np.linspace(0.0, 1.0, fade, endpoint=False)
    track[-fade:] *= np.linspace(1.0, 0.0, fade, endpoint=True)
    return np.asarray(np.clip(track, -1.0, 1.0) * 32767.0, dtype="<i2")


def tennis_power_on() -> np.ndarray:
    track = np.zeros(5 * SAMPLE_RATE, dtype=np.float64)

    # A light tennis double-bounce opens the cue without the previous hard,
    # metallic racket transient.
    for when, gain, pitch in ((0.06, 0.09, 294.0), (0.30, 0.07, 392.0)):
        add_noise_hit(track, when, 0.055, gain, 48.0)
        add_tone(track, when, 0.15, pitch, gain * 1.1, release=0.12)
    add_noise_hit(track, 0.50, 0.07, 0.08, 46.0)
    add_tone(track, 0.50, 0.20, 1046.50, 0.10, release=0.16)

    # Cheerful C-major/pentatonic hook at 160 BPM. Bell-like sine partials
    # keep it bright and friendly on the small loudspeaker without sounding
    # shrill like the former square-wave rise.
    eighth = 60.0 / 160.0 / 2.0
    notes = (
        523.25, 659.25, 783.99, 880.00,
        783.99, 659.25, 587.33, 659.25,
        783.99, 1046.50, 987.77, 880.00,
        783.99, 659.25, 783.99, 1046.50,
    )
    for index, note in enumerate(notes):
        when = 0.62 + index * eighth
        accent = 1.12 if index % 4 == 0 else 1.0
        add_tone(track, when, eighth * 0.90, note, 0.145 * accent, "sine", 0.006, 0.07)
        add_tone(track, when, eighth * 0.78, note * 2.0, 0.036, "sine", 0.004, 0.06)
        if index in (0, 4, 8, 12):
            add_noise_hit(track, when, 0.035, 0.025, 58.0)
            add_kick(track, when, 0.075)

    # Warm, smiling C-major finish with a short high sparkle.
    for frequency, gain in ((523.25, 0.14), (659.25, 0.12), (783.99, 0.11), (1046.50, 0.07)):
        add_tone(track, 3.72, 1.05, frequency, gain, "sine", 0.025, 0.62)
    for index, frequency in enumerate((1046.50, 1318.51, 1567.98, 2093.00)):
        add_tone(track, 4.05 + index * 0.095, 0.20, frequency, 0.045, "sine", 0.004, 0.12)
    return finish(track)


def power_on_candidate(style_index: int) -> np.ndarray:
    """Create five clearly different, original five-second startup sketches."""
    track = np.zeros(5 * SAMPLE_RATE, dtype=np.float64)
    arrangements = (
        # tempo, waveform, melody, bass/chord root, percussion character
        (148.0, "sine", (523.25, 659.25, 783.99, 659.25, 880.00, 783.99, 1046.50, 783.99), 130.81, 0.035),
        (132.0, "sine", (659.25, 783.99, 987.77, 1046.50, 987.77, 783.99, 659.25, 1046.50), 164.81, 0.020),
        (176.0, "square", (523.25, 659.25, 783.99, 1046.50, 783.99, 1046.50, 1318.51, 1567.98), 98.00, 0.045),
        (124.0, "sine", (392.00, 523.25, 659.25, 783.99, 659.25, 523.25, 783.99, 1046.50), 98.00, 0.060),
        (156.0, "saw", (440.00, 554.37, 659.25, 880.00, 739.99, 659.25, 987.77, 1318.51), 110.00, 0.030),
    )
    tempo, waveform, notes, root, noise_gain = arrangements[style_index]
    step = 60.0 / tempo / 2.0

    # Each candidate uses a different arrangement rather than pitch-shifting one cue.
    for index in range(18):
        when = 0.10 + index * step
        note = notes[index % len(notes)]
        if index % 4 == 0:
            add_kick(track, when, 0.16 if style_index != 3 else 0.25)
            add_tone(track, when, step * 1.8, root * (1.0 + (index // 8) * 0.25),
                     0.09, "sine", release=0.16)
        if style_index == 0:  # compact woody/marimba-like partials
            add_tone(track, when, step * 0.78, note, 0.16, "sine", release=0.055)
            add_tone(track, when, step * 0.45, note * 3.0, 0.035, "sine", release=0.035)
        elif style_index == 1:  # long sunshine bells
            add_tone(track, when, step * 1.45, note, 0.115, "sine", release=0.28)
            add_tone(track, when, step, note * 2.01, 0.042, "sine", release=0.20)
        elif style_index == 2:  # deliberately crisp 8-bit victory motif
            add_tone(track, when, step * 0.72, note, 0.115, waveform, release=0.025)
        elif style_index == 3:  # stadium clap/bounce with a broad melody
            add_tone(track, when, step * 0.88, note, 0.13, waveform, release=0.10)
            if index % 2:
                add_noise_hit(track, when, 0.07, noise_gain, 35.0)
        else:  # short futuristic synth sparks
            add_tone(track, when, step * 0.62, note, 0.095, waveform, release=0.045)
            add_tone(track, when + 0.025, step * 0.52, note * 1.5, 0.055, "sine", release=0.04)
        if style_index != 1 and index % 2:
            add_noise_hit(track, when, 0.035, noise_gain, 52.0)

    # A style-specific closing chord occupies the final beat and ensures a clean finish.
    chord = ((1.0, 1.25, 1.5), (1.0, 1.2, 1.5), (1.0, 1.25, 2.0),
             (1.0, 4.0 / 3.0, 5.0 / 3.0), (1.0, 1.5, 2.25))[style_index]
    for ratio in chord:
        add_tone(track, 4.25, 0.70, 523.25 * ratio, 0.085, "sine", release=0.42)
    return finish(track)


def robot_dance() -> np.ndarray:
    track = np.zeros(12 * SAMPLE_RATE, dtype=np.float64)
    eighth = 0.25  # 120 BPM, eighth-note grid; covers the 45-step gait plus recovery.
    bass_notes = (65.41, 77.78, 98.00, 87.31)
    lead_notes = (261.63, 311.13, 392.00, 466.16, 392.00, 311.13, 523.25, 392.00)

    for step in range(48):
        when = step * eighth
        if step % 2 == 0:
            add_kick(track, when, 0.30)
            bass = bass_notes[(step // 4) % len(bass_notes)]
            add_tone(track, when, 0.30, bass, 0.18, "square", 0.004, 0.10)
        else:
            add_noise_hit(track, when, 0.045, 0.055, 55.0)
        if step % 4 in (1, 3):
            add_noise_hit(track, when, 0.11, 0.10, 26.0)

        lead = lead_notes[step % len(lead_notes)]
        # Alternating octave jumps make the melody deliberately robotic.
        if 12 <= step < 24 or step >= 40:
            add_tone(track, when + 0.015, 0.18, lead, 0.105, "square", 0.004, 0.055)
            if step & 1:
                add_tone(track, when + 0.015, 0.16, lead * 2.0, 0.045, "sine", 0.004, 0.05)

    # Metallic transition sweeps mark the diagonal and hip sections.
    for when in (1.0, 3.0, 6.0, 8.0, 10.0):
        for index in range(7):
            add_tone(track, when + index * 0.035, 0.10, 480.0 + index * 105.0,
                     0.035, "sine", 0.002, 0.06)
    # The final half-second chord accompanies the two-stage four-foot recovery.
    add_tone(track, 11.50, 0.46, 523.25, 0.11, "square", release=0.30)
    add_tone(track, 11.50, 0.46, 783.99, 0.09, "sine", release=0.30)
    return finish(track)


def encode_mp3(samples: np.ndarray, path: Path) -> None:
    encoder = lameenc.Encoder()
    encoder.set_bit_rate(48)
    encoder.set_in_sample_rate(SAMPLE_RATE)
    encoder.set_channels(1)
    encoder.set_quality(2)
    data = encoder.encode(samples.tobytes()) + encoder.flush()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        default=Path("res/arcs-mini/tone"))
    args = parser.parse_args()
    encode_mp3(tennis_power_on(), args.output_dir / "106_power_on.mp3")
    encode_mp3(robot_dance(), args.output_dir / "110_robot_dance.mp3")
    candidate_dir = args.output_dir / "power_on_candidates"
    for index, style in enumerate(POWER_ON_CANDIDATE_STYLES):
        encode_mp3(power_on_candidate(index), candidate_dir / f"{style}.mp3")
    print(args.output_dir / "106_power_on.mp3")
    print(args.output_dir / "110_robot_dance.mp3")
    for style in POWER_ON_CANDIDATE_STYLES:
        print(candidate_dir / f"{style}.mp3")


if __name__ == "__main__":
    main()
