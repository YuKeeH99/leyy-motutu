# Local speech onset detector

Vendored from https://github.com/dpirch/libfvad (master archive, fetched 2026-09-20).
Archive SHA-256: `331c5a7a10005c465298746341569fce2416713f5ad93d15015f448d623986ad`.
Upstream source is unmodified; LICENSE, PATENTS and AUTHORS accompany it.

The CP application uses the static WebRTC VAD core, 16 kHz / 10 ms / mode 2,
only to start the twelve-second utterance budget. Audio still streams regardless
of VAD classification. After speech onset, VAD processing stops for that round.
Retain the accompanying license in distributed firmware materials.
