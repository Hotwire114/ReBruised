import wave
import numpy as np


def load(path):
    if path.lower().endswith(".wav"):
        with wave.open(path, "rb") as w:
            rate = w.getframerate()
            ch = w.getnchannels()
            data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch)
            return data.astype(np.float64), rate
    data = np.fromfile(path, dtype="<i2")
    return data[: len(data) // 2 * 2].reshape(-1, 2).astype(np.float64), 48000
