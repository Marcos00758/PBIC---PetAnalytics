import serial
import struct
import time
from pathlib import Path
import numpy as np
import matplotlib.pyplot as plt

PORT      = "COM3"
BAUD      = 115200
OUTFILE   = Path("imu.bin")

MAGIC       = 0xAA55
# int16 raw: H=magic, I=timestamp, 9x h=int16 raw, H=seq, B=crc8
PACKET_FMT  = "<HIhhhhhhhhhHB"
PACKET_SIZE = struct.calcsize(PACKET_FMT)

# ── Fatores de escala corrigidos (fonte: Edge Impulse firmware) ───────────────
# Acelerômetro: range +/-2g, 16-bit signed
ACCEL_SCALE = (2.0 * 9.80665) / 32767.5      # m/s² por LSB
# Giroscópio: range 1000dps (reg 0x43=0x11), 16-bit signed
GYRO_SCALE  = 1000.0 / 32768.0               # °/s por LSB  (era 1/16.4 — ERRADO)
# Magnetômetro XY: 13 bits após >>3, range ±1300µT sobre 4096 níveis
MAG_XY_SCALE = 1300.0 / 4096.0              # µT por LSB
# Magnetômetro Z: 15 bits após >>1, range ±2500µT sobre 16384 níveis
MAG_Z_SCALE  = 2500.0 / 16384.0             # µT por LSB

def crc8(data: bytes) -> int:
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc

def capture_binary_stream(duration_s: int = 20) -> None:
    with serial.Serial(PORT, BAUD, timeout=1.0) as ser:
        print("Aguardando IMU_READY...")
        while True:
            line = ser.readline().decode(errors="ignore").strip()
            if not line:
                continue
            print("[Arduino]", line)
            if line == "IMU_READY":
                break

        time.sleep(0.2)
        ser.write(b'S')
        print("Comando S enviado — aguardando STREAM_BIN_START...")

        while True:
            line = ser.readline().decode(errors="ignore").strip()
            if not line:
                continue
            print("[Arduino]", line)
            if line == "STREAM_BIN_START":
                break

        ser.reset_input_buffer()
        raw = bytearray()
        t0  = time.monotonic()
        print(f"Capturando por {duration_s}s  |  pacote={PACKET_SIZE} bytes ...")

        while time.monotonic() - t0 < duration_s:
            chunk = ser.read(256)
            if chunk:
                raw.extend(chunk)

    OUTFILE.write_bytes(raw)
    print(f"Salvo: {OUTFILE}  ({len(raw)} bytes, ~{len(raw)//PACKET_SIZE} pacotes)")

def parse_packets(data: bytes):
    packets, bad_crc, bad_magic, gaps = [], 0, 0, []
    i = 0
    last_seq = None

    while i + PACKET_SIZE <= len(data):
        magic, = struct.unpack_from("<H", data, i)
        if magic != MAGIC:
            bad_magic += 1
            i += 1
            continue

        pkt = struct.unpack_from(PACKET_FMT, data, i)
        if crc8(data[i : i + PACKET_SIZE - 1]) != pkt[-1]:
            bad_crc += 1
            i += 1
            continue

        seq = pkt[-2]
        if last_seq is not None:
            expected = (last_seq + 1) & 0xFFFF
            if seq != expected:
                gaps.append((seq, (seq - last_seq - 1) & 0xFFFF))
        last_seq = seq
        packets.append(pkt)
        i += PACKET_SIZE

    print(f"Pacotes válidos : {len(packets)}")
    print(f"Magic inválido  : {bad_magic}")
    print(f"CRC inválido    : {bad_crc}")
    if gaps:
        print(f"Gaps detectados : {len(gaps)}  (perdidos ≈ {sum(g for _,g in gaps)})")
    else:
        print("Sem gaps — nenhum pacote perdido!")
    return packets

def apply_scales(arr):
    """Converte raw int16 para unidades físicas."""
    return {
        "t":  (arr["timestamp_us"] - arr["timestamp_us"][0]) / 1e6,
        "ax": arr["ax"] * ACCEL_SCALE,
        "ay": arr["ay"] * ACCEL_SCALE,
        "az": arr["az"] * ACCEL_SCALE,
        "gx": arr["gx"] * GYRO_SCALE,
        "gy": arr["gy"] * GYRO_SCALE,
        "gz": arr["gz"] * GYRO_SCALE,
        "mx": arr["mx"] * MAG_XY_SCALE,
        "my": arr["my"] * MAG_XY_SCALE,
        "mz": arr["mz"] * MAG_Z_SCALE,
    }

def plot(arr):
    d = apply_scales(arr)
    t = d["t"]
    dt = np.diff(t)
    fs = 1.0 / np.median(dt)
    jitter_ms = np.std(dt) * 1000

    fig, axes = plt.subplots(3, 1, figsize=(14, 9), sharex=True)

    axes[0].plot(t, d["ax"], label="ax")
    axes[0].plot(t, d["ay"], label="ay")
    axes[0].plot(t, d["az"], label="az")
    axes[0].set_ylabel("Aceleração (m/s²)")
    axes[0].legend(); axes[0].grid(True)

    axes[1].plot(t, d["gx"], label="gx")
    axes[1].plot(t, d["gy"], label="gy")
    axes[1].plot(t, d["gz"], label="gz")
    axes[1].set_ylabel("Giroscópio (°/s)")
    axes[1].legend(); axes[1].grid(True)

    axes[2].plot(t, d["mx"], label="mx")
    axes[2].plot(t, d["my"], label="my")
    axes[2].plot(t, d["mz"], label="mz")
    axes[2].set_ylabel("Magnetômetro (µT)")
    axes[2].set_xlabel("Tempo (s)")
    axes[2].legend(); axes[2].grid(True)

    fig.suptitle(
        f"IMU 9-eixos — {len(arr)} pkts | fs≈{fs:.1f} Hz | jitter≈{jitter_ms:.2f} ms\n"
        f"Gyro range: ±1000°/s | Accel range: ±2g"
    )
    plt.tight_layout()
    plt.show()

def main():
    capture_binary_stream(duration_s=20)

    data    = OUTFILE.read_bytes()
    packets = parse_packets(data)
    if not packets:
        print("Nenhum pacote válido encontrado.")
        return

    arr = np.array(packets, dtype=np.dtype([
        ("magic",        np.uint16),
        ("timestamp_us", np.uint32),
        ("ax", np.int16), ("ay", np.int16), ("az", np.int16),
        ("gx", np.int16), ("gy", np.int16), ("gz", np.int16),
        ("mx", np.int16), ("my", np.int16), ("mz", np.int16),
        ("seq",  np.uint16),
        ("crc8", np.uint8),
    ]))

    # Imprime sanity check com o sensor parado
    d = apply_scales(arr)
    print(f"\n── Sanity check (médias) ──────────────────")
    print(f"Accel:  ax={d['ax'].mean():.3f}  ay={d['ay'].mean():.3f}  az={d['az'].mean():.3f} m/s²")
    print(f"Gyro:   gx={d['gx'].mean():.3f}  gy={d['gy'].mean():.3f}  gz={d['gz'].mean():.3f} °/s")
    print(f"Mag:    mx={d['mx'].mean():.2f}  my={d['my'].mean():.2f}  mz={d['mz'].mean():.2f} µT")
    print(f"──────────────────────────────────────────")
    print(f"Esperado parado: |az|≈9.8 m/s², |gx,gy,gz|≈0, mag varia por ambiente")

    plot(arr)

if __name__ == "__main__":
    main()