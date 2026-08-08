import struct
import sys
from pathlib import Path
import numpy as np
import matplotlib.pyplot as plt

# Caminho do .bin a analisar. Pode ser:
#  - caminho direto do imu.bin no SD (ex: "E:/S001/imu.bin" no Windows)
#  - cópia local (ex: "imu_int.bin")
#  - argumento da linha de comando: python testIMUICM.py "E:/S003/imu.bin"
INFILE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("imu_int.bin")

MAGIC = 0xAA55
# 2 + 4 + 27×int16 + 4×uint32 + 2 + 1 = 79 bytes
# (BMI270 + BMM150 + 3× ICM-20948 + 2× BMP390 raw press/temp 24-bit)
PACKET_FMT  = "<HI" + "h" * 27 + "I" * 4 + "HB"
PACKET_SIZE = struct.calcsize(PACKET_FMT)
assert PACKET_SIZE == 79, f"Tamanho inesperado: {PACKET_SIZE}"

# ── Fatores de escala (aplicados aqui, dados chegam raw int16) ───────────────
BMI_ACCEL_SCALE = (2.0 * 9.80665) / 32767.5   # ±2g → m/s²
BMI_GYRO_SCALE  = 1000.0 / 32768.0             # ±1000dps → °/s
MAG_XY_SCALE    = 1300.0 / 4096.0              # XY já vem com >>3 → µT
MAG_Z_SCALE     = 2500.0 / 16384.0             # Z  já vem com >>1 → µT
ICM_ACCEL_SCALE = (2.0 * 9.80665) / 32767.5
ICM_GYRO_SCALE  = 250.0 / 32768.0


# ── BMP390: compensação Bosch (coeficientes NVM lidos do meta.txt) ───────────
def bmp390_coeffs(nvm: bytes) -> dict:
    """Converte os 21 bytes de NVM trim nos coeficientes float (datasheet Bosch)."""
    t1  = struct.unpack_from("<H", nvm, 0)[0]
    t2  = struct.unpack_from("<H", nvm, 2)[0]
    t3  = struct.unpack_from("<b", nvm, 4)[0]
    p1  = struct.unpack_from("<h", nvm, 5)[0]
    p2  = struct.unpack_from("<h", nvm, 7)[0]
    p3  = struct.unpack_from("<b", nvm, 9)[0]
    p4  = struct.unpack_from("<b", nvm, 10)[0]
    p5  = struct.unpack_from("<H", nvm, 11)[0]
    p6  = struct.unpack_from("<H", nvm, 13)[0]
    p7  = struct.unpack_from("<b", nvm, 15)[0]
    p8  = struct.unpack_from("<b", nvm, 16)[0]
    p9  = struct.unpack_from("<h", nvm, 17)[0]
    p10 = struct.unpack_from("<b", nvm, 19)[0]
    p11 = struct.unpack_from("<b", nvm, 20)[0]
    return dict(
        t1=t1 / 2**-8, t2=t2 / 2**30, t3=t3 / 2**48,
        p1=(p1 - 16384) / 2**20, p2=(p2 - 16384) / 2**29,
        p3=p3 / 2**32, p4=p4 / 2**37, p5=p5 / 2**-3, p6=p6 / 2**6,
        p7=p7 / 2**8, p8=p8 / 2**15, p9=p9 / 2**48, p10=p10 / 2**48,
        p11=p11 / 2**65,
    )


def bmp390_compensate(raw_press, raw_temp, c):
    """raw 24-bit → (pressão em Pa, temperatura em °C). Vetorizado (numpy)."""
    up = np.asarray(raw_press, dtype=np.float64)
    ut = np.asarray(raw_temp, dtype=np.float64)

    # temperatura
    pd1 = ut - c["t1"]
    t_lin = (pd1 * c["t2"]) + (pd1 * pd1) * c["t3"]

    # pressão
    out1 = c["p5"] + c["p6"] * t_lin + c["p7"] * t_lin**2 + c["p8"] * t_lin**3
    out2 = up * (c["p1"] + c["p2"] * t_lin + c["p3"] * t_lin**2 + c["p4"] * t_lin**3)
    pd = up * up * (c["p9"] + c["p10"] * t_lin) + (up**3) * c["p11"]
    press = out1 + out2 + pd
    return press, t_lin


def load_meta(meta_path: Path) -> dict:
    """Lê coeficientes NVM dos BMP390 do meta.txt → {'bmp0': coeffs, 'bmp1': coeffs}."""
    out = {}
    if not meta_path.exists():
        return out
    for line in meta_path.read_text(errors="ignore").splitlines():
        if "=" not in line:
            continue
        k, v = (s.strip() for s in line.split("=", 1))
        if k in ("bmp0_nvm", "bmp1_nvm"):
            try:
                nvm = bytes.fromhex(v)
            except ValueError:
                continue
            if len(nvm) == 21:
                out["bmp" + k[3]] = bmp390_coeffs(nvm)
    return out


def crc8(data: bytes) -> int:
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


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


def to_dataframe(packets, meta=None):
    """Converte pacotes raw para array estruturado em unidades físicas.
    meta: dict com coeficientes BMP390 ({'bmp0':..., 'bmp1':...}) do meta.txt."""
    meta = meta or {}
    raw_dtype = np.dtype([
        ("magic",        np.uint16),
        ("timestamp_us", np.uint32),
        ("ax_i", np.int16), ("ay_i", np.int16), ("az_i", np.int16),
        ("gx_i", np.int16), ("gy_i", np.int16), ("gz_i", np.int16),
        ("mx_i", np.int16), ("my_i", np.int16), ("mz_i", np.int16),
        ("ax_0", np.int16), ("ay_0", np.int16), ("az_0", np.int16),
        ("gx_0", np.int16), ("gy_0", np.int16), ("gz_0", np.int16),
        ("ax_1", np.int16), ("ay_1", np.int16), ("az_1", np.int16),
        ("gx_1", np.int16), ("gy_1", np.int16), ("gz_1", np.int16),
        ("ax_2", np.int16), ("ay_2", np.int16), ("az_2", np.int16),
        ("gx_2", np.int16), ("gy_2", np.int16), ("gz_2", np.int16),
        ("press_0", np.uint32), ("temp_0", np.uint32),
        ("press_1", np.uint32), ("temp_1", np.uint32),
        ("seq",  np.uint16),
        ("crc8", np.uint8),
    ])
    raw = np.array(packets, dtype=raw_dtype)

    # Array final em unidades físicas (float32)
    out_dtype = np.dtype([
        ("timestamp_us", np.uint32),
        ("ax_i", np.float32), ("ay_i", np.float32), ("az_i", np.float32),
        ("gx_i", np.float32), ("gy_i", np.float32), ("gz_i", np.float32),
        ("mx_i", np.float32), ("my_i", np.float32), ("mz_i", np.float32),
        ("ax_0", np.float32), ("ay_0", np.float32), ("az_0", np.float32),
        ("gx_0", np.float32), ("gy_0", np.float32), ("gz_0", np.float32),
        ("ax_1", np.float32), ("ay_1", np.float32), ("az_1", np.float32),
        ("gx_1", np.float32), ("gy_1", np.float32), ("gz_1", np.float32),
        ("ax_2", np.float32), ("ay_2", np.float32), ("az_2", np.float32),
        ("gx_2", np.float32), ("gy_2", np.float32), ("gz_2", np.float32),
        ("press0", np.float32), ("temp0", np.float32),  # BMP#0 Pa / °C
        ("press1", np.float32), ("temp1", np.float32),  # BMP#1 Pa / °C
        ("seq",  np.uint16),
    ])
    arr = np.zeros(len(raw), dtype=out_dtype)
    arr["timestamp_us"] = raw["timestamp_us"]
    arr["seq"]          = raw["seq"]

    for ax in ("ax_i", "ay_i", "az_i"): arr[ax] = raw[ax] * BMI_ACCEL_SCALE
    for ax in ("gx_i", "gy_i", "gz_i"): arr[ax] = raw[ax] * BMI_GYRO_SCALE
    for ax in ("mx_i", "my_i"):         arr[ax] = raw[ax] * MAG_XY_SCALE
    arr["mz_i"] = raw["mz_i"] * MAG_Z_SCALE
    for ax in ("ax_0", "ay_0", "az_0"): arr[ax] = raw[ax] * ICM_ACCEL_SCALE
    for ax in ("gx_0", "gy_0", "gz_0"): arr[ax] = raw[ax] * ICM_GYRO_SCALE
    for ax in ("ax_1", "ay_1", "az_1"): arr[ax] = raw[ax] * ICM_ACCEL_SCALE
    for ax in ("gx_1", "gy_1", "gz_1"): arr[ax] = raw[ax] * ICM_GYRO_SCALE
    for ax in ("ax_2", "ay_2", "az_2"): arr[ax] = raw[ax] * ICM_ACCEL_SCALE
    for ax in ("gx_2", "gy_2", "gz_2"): arr[ax] = raw[ax] * ICM_GYRO_SCALE

    # ── BMP390: compensação Bosch (Pa, °C) se houver coeficientes no meta.txt ──
    for idx, (pcol, tcol, praw, traw) in enumerate([
        ("press0", "temp0", "press_0", "temp_0"),
        ("press1", "temp1", "press_1", "temp_1"),
    ]):
        c = meta.get(f"bmp{idx}")
        if c is not None:
            p, tdeg = bmp390_compensate(raw[praw], raw[traw], c)
            arr[pcol] = p
            arr[tcol] = tdeg
        else:
            # sem coeficientes: deixa raw (escala errada, mas visível)
            arr[pcol] = raw[praw]
            arr[tcol] = raw[traw]

    return arr


def plot(arr):
    t = (arr["timestamp_us"] - arr["timestamp_us"][0]) / 1e6
    dt = np.diff(t)
    fs = 1.0 / np.median(dt)
    jitter_ms = np.std(dt) * 1000

    fig, axes = plt.subplots(4, 4, figsize=(22, 14), sharex=True)
    fig.suptitle(
        f"IMU — {len(arr)} pkts | fs≈{fs:.1f} Hz | jitter≈{jitter_ms:.2f} ms\n"
        f"Col 1: BMI270 (interno) | Col 2: ICM#0 (ch0) | Col 3: ICM#1 (ch1) | Col 4: ICM#2 (ch4)"
    )

    # ── Linha 0: Acelerômetro ────────────────────────────────────────────────
    for ax, px, py, pz, title in [
        (axes[0, 0], "ax_i", "ay_i", "az_i", "BMI270 — Acelerômetro"),
        (axes[0, 1], "ax_0", "ay_0", "az_0", "ICM#0 — Acelerômetro"),
        (axes[0, 2], "ax_1", "ay_1", "az_1", "ICM#1 — Acelerômetro"),
        (axes[0, 3], "ax_2", "ay_2", "az_2", "ICM#2 — Acelerômetro"),
    ]:
        ax.plot(t, arr[px], label="ax")
        ax.plot(t, arr[py], label="ay")
        ax.plot(t, arr[pz], label="az")
        ax.set_ylabel("Acel (m/s²)")
        ax.set_title(title)
        ax.legend(fontsize=8); ax.grid(True)

    # ── Linha 1: Giroscópio ──────────────────────────────────────────────────
    for ax, px, py, pz, title in [
        (axes[1, 0], "gx_i", "gy_i", "gz_i", "BMI270 — Giroscópio"),
        (axes[1, 1], "gx_0", "gy_0", "gz_0", "ICM#0 — Giroscópio"),
        (axes[1, 2], "gx_1", "gy_1", "gz_1", "ICM#1 — Giroscópio"),
        (axes[1, 3], "gx_2", "gy_2", "gz_2", "ICM#2 — Giroscópio"),
    ]:
        ax.plot(t, arr[px], label="gx")
        ax.plot(t, arr[py], label="gy")
        ax.plot(t, arr[pz], label="gz")
        ax.set_ylabel("Giro (°/s)")
        ax.set_title(title)
        ax.legend(fontsize=8); ax.grid(True)

    # ── Linha 2: Magnetômetro (só BMM150 — os ICMs não têm mag implementado) ──
    axes[2, 0].plot(t, arr["mx_i"], label="mx")
    axes[2, 0].plot(t, arr["my_i"], label="my")
    axes[2, 0].plot(t, arr["mz_i"], label="mz")
    axes[2, 0].set_ylabel("Mag (µT)")
    axes[2, 0].set_xlabel("Tempo (s)")
    axes[2, 0].set_title("BMM150 — Magnetômetro")
    axes[2, 0].legend(fontsize=8); axes[2, 0].grid(True)

    # Células vazias: ICMs não fornecem magnetômetro no firmware atual
    for ax, sensor in [(axes[2, 1], "ICM#0"), (axes[2, 2], "ICM#1"), (axes[2, 3], "ICM#2")]:
        ax.set_title(f"{sensor} — Magnetômetro (não implementado)")
        ax.text(0.5, 0.5, "AK09916\nnão implementado", ha="center", va="center",
                transform=ax.transAxes, fontsize=10, color="gray")
        ax.grid(True)

    # ── Linha 3: BMP390 — pressão e temperatura (compensadas no host) ─────────
    axes[3, 0].plot(t, arr["press0"], color="tab:blue")
    axes[3, 0].set_ylabel("Pressão (Pa)"); axes[3, 0].set_xlabel("Tempo (s)")
    axes[3, 0].set_title("BMP#0 (ch2) — Pressão"); axes[3, 0].grid(True)

    axes[3, 1].plot(t, arr["temp0"], color="tab:red")
    axes[3, 1].set_ylabel("Temp (°C)"); axes[3, 1].set_xlabel("Tempo (s)")
    axes[3, 1].set_title("BMP#0 (ch2) — Temperatura"); axes[3, 1].grid(True)

    axes[3, 2].plot(t, arr["press1"], color="tab:blue")
    axes[3, 2].set_ylabel("Pressão (Pa)"); axes[3, 2].set_xlabel("Tempo (s)")
    axes[3, 2].set_title("BMP#1 (ch3) — Pressão"); axes[3, 2].grid(True)

    axes[3, 3].plot(t, arr["temp1"], color="tab:red")
    axes[3, 3].set_ylabel("Temp (°C)"); axes[3, 3].set_xlabel("Tempo (s)")
    axes[3, 3].set_title("BMP#1 (ch3) — Temperatura"); axes[3, 3].grid(True)

    plt.tight_layout()
    plt.show()

    print(f"\n── Sanity check (médias, sensor parado) ──────────")
    print(f"BMI270   accel: az={arr['az_i'].mean():.3f} m/s² (esperado ≈ ±9.8)")
    print(f"ICM#0    accel: az={arr['az_0'].mean():.3f} m/s² (esperado ≈ ±9.8)")
    print(f"ICM#1    accel: az={arr['az_1'].mean():.3f} m/s² (esperado ≈ ±9.8)")
    print(f"ICM#2    accel: az={arr['az_2'].mean():.3f} m/s² (esperado ≈ ±9.8)")
    print(f"BMI270   gyro:  gz={arr['gz_i'].mean():.3f} °/s  (esperado ≈ 0)")
    print(f"ICM#0    gyro:  gz={arr['gz_0'].mean():.3f} °/s  (esperado ≈ 0)")
    print(f"ICM#1    gyro:  gz={arr['gz_1'].mean():.3f} °/s  (esperado ≈ 0)")
    print(f"ICM#2    gyro:  gz={arr['gz_2'].mean():.3f} °/s  (esperado ≈ 0)")
    print(f"BMP#0:  press={arr['press0'].mean()/100:.2f} hPa  temp={arr['temp0'].mean():.2f} °C")
    print(f"BMP#1:  press={arr['press1'].mean()/100:.2f} hPa  temp={arr['temp1'].mean():.2f} °C")
    print(f"        (esperado ≈ 1013 hPa ao nível do mar)")


def main():
    if not INFILE.exists():
        print(f"Arquivo nao encontrado: {INFILE}")
        print("Uso: python testIMUICM.py <caminho/para/imu.bin>")
        return

    data = INFILE.read_bytes()
    print(f"Lido: {INFILE}  ({len(data)} bytes, ~{len(data)//PACKET_SIZE} pacotes)")

    # meta.txt (mesma pasta do imu.bin) traz os coeficientes NVM do BMP390
    meta = load_meta(INFILE.parent / "meta.txt")
    if "bmp0" not in meta and "bmp1" not in meta:
        print("AVISO: meta.txt sem coeficientes BMP390 — pressão/temp ficarão em "
              "valores brutos (escala errada). Copie o meta.txt junto do imu.bin.")

    packets = parse_packets(data)
    if not packets:
        print("Nenhum pacote válido encontrado.")
        return

    arr = to_dataframe(packets, meta)
    plot(arr)


if __name__ == "__main__":
    main()
