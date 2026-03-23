
import argparse
import math
import os
import shutil
import struct
import subprocess
import tempfile
from typing import List, Optional

import cv2
import numpy as np
from PIL import Image

MAGIC = b"CRPTVF1"
HEADER_FMT = "<8sHHHIHBBBBIIIII20s"
HEADER_SIZE = 64
SUPPORTED_PALETTES = (8, 16, 64, 128, 256)

AUDIO_CODEC_NONE = 0
AUDIO_CODEC_PCM_U8 = 1


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_hi_lo_bytes(c: int) -> bytes:
    return bytes([(c >> 8) & 0xFF, c & 0xFF])


def build_header(
    width: int,
    height: int,
    fps: int,
    frame_count: int,
    palette_size: int,
    audio_codec: int,
    audio_channels: int,
    audio_sample_rate: int,
    video_offset: int,
    video_size: int,
    audio_offset: int,
    audio_size: int,
) -> bytes:
    return struct.pack(
        HEADER_FMT,
        MAGIC + b"\x00",
        width,
        height,
        fps,
        frame_count,
        palette_size,
        8,              # bits_per_pixel
        0,              # flags
        audio_codec,
        audio_channels,
        audio_sample_rate,
        video_offset,
        video_size,
        audio_offset,
        audio_size,
        b"\x00" * 20,
    )


def bgr_to_pil_rgb(frame_bgr: np.ndarray) -> Image.Image:
    frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
    return Image.fromarray(frame_rgb)


def resize_with_fit(img: Image.Image, width: int, height: int, pad_color=(0, 0, 0)) -> Image.Image:
    src_w, src_h = img.size
    scale = min(width / src_w, height / src_h)
    new_w = max(1, int(src_w * scale))
    new_h = max(1, int(src_h * scale))

    resized = img.resize((new_w, new_h), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (width, height), pad_color)
    x = (width - new_w) // 2
    y = (height - new_h) // 2
    canvas.paste(resized, (x, y))
    return canvas


def make_palette_image(frames_rgb: List[Image.Image], palette_size: int, sample_limit: int = 64) -> Image.Image:
    if not frames_rgb:
        raise ValueError("No hay frames para generar paleta")

    used = frames_rgb[:sample_limit]
    n = len(used)
    cols = math.ceil(math.sqrt(n))
    rows = math.ceil(n / cols)

    thumb_w, thumb_h = used[0].size
    sheet = Image.new("RGB", (cols * thumb_w, rows * thumb_h))

    for i, frame in enumerate(used):
        x = (i % cols) * thumb_w
        y = (i // cols) * thumb_h
        sheet.paste(frame, (x, y))

    paletted = sheet.quantize(colors=palette_size, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    palette_img = Image.new("P", (1, 1))
    palette_img.putpalette(paletted.getpalette()[:768])
    return palette_img


def palette_rgb565_bytes_from_palette_image(palette_img: Image.Image, palette_size: int) -> bytes:
    raw_palette = palette_img.getpalette()
    if raw_palette is None:
        raise ValueError("No se pudo obtener la paleta")

    out = bytearray()
    for i in range(palette_size):
        base = i * 3
        r = raw_palette[base] if base < len(raw_palette) else 0
        g = raw_palette[base + 1] if base + 1 < len(raw_palette) else 0
        b = raw_palette[base + 2] if base + 2 < len(raw_palette) else 0
        out.extend(rgb565_hi_lo_bytes(rgb888_to_rgb565(r, g, b)))
    return bytes(out)


def extract_frames(
    input_path: str,
    width: int,
    height: int,
    target_fps: int,
    max_frames: Optional[int] = None,
) -> List[Image.Image]:
    cap = cv2.VideoCapture(input_path)
    if not cap.isOpened():
        raise RuntimeError(f"No se pudo abrir el video: {input_path}")

    source_fps = cap.get(cv2.CAP_PROP_FPS)
    if not source_fps or source_fps <= 0 or math.isnan(source_fps):
        source_fps = 30.0

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    duration_sec = total_frames / source_fps if total_frames > 0 else 0
    expected_out = int(duration_sec * target_fps) if duration_sec > 0 else 0

    print(f"FPS origen: {source_fps:.3f}")
    print(f"Frames origen: {total_frames}")
    print(f"Duración aprox: {duration_sec:.2f} s")
    print(f"Frames salida estimados: {expected_out}")

    frames: List[Image.Image] = []
    written_frames = 0
    next_output_time = 0.0

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        current_frame_index = cap.get(cv2.CAP_PROP_POS_FRAMES) - 1
        current_time = current_frame_index / source_fps

        if current_time + (1.0 / source_fps) / 2 < next_output_time:
            continue

        pil_img = bgr_to_pil_rgb(frame)
        fitted = resize_with_fit(pil_img, width, height, pad_color=(0, 0, 0))
        frames.append(fitted)

        written_frames += 1
        next_output_time = written_frames / target_fps

        if max_frames is not None and written_frames >= max_frames:
            break

        if written_frames % 25 == 0:
            print(f"Frames extraídos: {written_frames}")

    cap.release()

    if not frames:
        raise RuntimeError("No se extrajeron frames")

    print(f"Frames extraídos reales: {len(frames)}")
    return frames


def convert_frames_to_indexed(frames_rgb: List[Image.Image], palette_img: Image.Image) -> List[bytes]:
    indexed_frames: List[bytes] = []
    total = len(frames_rgb)

    for i, frame in enumerate(frames_rgb, start=1):
        indexed = frame.quantize(palette=palette_img, dither=Image.Dither.NONE)
        indexed_frames.append(indexed.tobytes())

        if i % 25 == 0 or i == total:
            print(f"Frames indexados: {i}/{total}")

    return indexed_frames


def ensure_ffmpeg() -> None:
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("No se encontró ffmpeg en PATH. Instálalo para embebeder audio.")


def extract_audio_pcm_u8(input_path: str, sample_rate: int = 8000, channels: int = 1) -> bytes:
    ensure_ffmpeg()

    with tempfile.NamedTemporaryFile(delete=False, suffix=".u8") as tmp:
        tmp_path = tmp.name

    cmd = [
        "ffmpeg",
        "-y",
        "-i", input_path,
        "-vn",
        "-acodec", "pcm_u8",
        "-f", "u8",
        "-ac", str(channels),
        "-ar", str(sample_rate),
        tmp_path,
    ]

    print("Extrayendo audio:", " ".join(cmd))
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    except subprocess.CalledProcessError as e:
        stderr = e.stderr.decode("utf-8", errors="ignore")
        raise RuntimeError(f"ffmpeg falló al extraer audio:\n{stderr}")

    with open(tmp_path, "rb") as f:
        audio_data = f.read()

    try:
        os.remove(tmp_path)
    except OSError:
        pass

    print(f"Audio extraído: {len(audio_data)} bytes")
    return audio_data


def write_crptvf(
    output_path: str,
    width: int,
    height: int,
    fps: int,
    palette_size: int,
    palette_bytes: bytes,
    indexed_frames: List[bytes],
    audio_codec: int,
    audio_channels: int,
    audio_sample_rate: int,
    audio_bytes: bytes,
) -> None:
    video_offset = HEADER_SIZE + len(palette_bytes)
    frame_bytes_total = len(indexed_frames) * (width * height)
    video_size = frame_bytes_total

    if audio_bytes:
        audio_offset = video_offset + video_size
        audio_size = len(audio_bytes)
    else:
        audio_offset = 0
        audio_size = 0

    header = build_header(
        width=width,
        height=height,
        fps=fps,
        frame_count=len(indexed_frames),
        palette_size=palette_size,
        audio_codec=audio_codec,
        audio_channels=audio_channels,
        audio_sample_rate=audio_sample_rate,
        video_offset=video_offset,
        video_size=video_size,
        audio_offset=audio_offset,
        audio_size=audio_size,
    )

    with open(output_path, "wb") as f:
        f.write(header)
        f.write(palette_bytes)
        for frame_data in indexed_frames:
            f.write(frame_data)
        if audio_bytes:
            f.write(audio_bytes)

    expected_size = HEADER_SIZE + len(palette_bytes) + video_size + audio_size
    real_size = os.path.getsize(output_path)

    print(f"Archivo generado: {output_path}")
    print(f"Tamaño esperado: {expected_size} bytes")
    print(f"Tamaño real    : {real_size} bytes")


def convert_mp4_to_crptvf(
    input_path: str,
    output_path: str,
    width: int,
    height: int,
    fps: int,
    palette_size: int,
    palette_samples: int,
    with_audio: bool,
    audio_sample_rate: int,
    audio_channels: int,
    max_frames: Optional[int] = None,
) -> None:
    if palette_size not in SUPPORTED_PALETTES:
        raise ValueError(f"palette_size debe ser uno de {SUPPORTED_PALETTES}")

    if not os.path.isfile(input_path):
        raise FileNotFoundError(f"No existe el archivo de entrada: {input_path}")

    frames_rgb = extract_frames(
        input_path=input_path,
        width=width,
        height=height,
        target_fps=fps,
        max_frames=max_frames,
    )

    print("Generando paleta global...")
    palette_img = make_palette_image(frames_rgb, palette_size=palette_size, sample_limit=palette_samples)
    palette_bytes = palette_rgb565_bytes_from_palette_image(palette_img, palette_size)

    print("Convirtiendo frames a índices...")
    indexed_frames = convert_frames_to_indexed(frames_rgb, palette_img)

    audio_codec = AUDIO_CODEC_NONE
    audio_bytes = b""
    if with_audio:
        audio_bytes = extract_audio_pcm_u8(
            input_path=input_path,
            sample_rate=audio_sample_rate,
            channels=audio_channels,
        )
        audio_codec = AUDIO_CODEC_PCM_U8

    write_crptvf(
        output_path=output_path,
        width=width,
        height=height,
        fps=fps,
        palette_size=palette_size,
        palette_bytes=palette_bytes,
        indexed_frames=indexed_frames,
        audio_codec=audio_codec,
        audio_channels=audio_channels if with_audio else 0,
        audio_sample_rate=audio_sample_rate if with_audio else 0,
        audio_bytes=audio_bytes,
    )

    print("Conversión terminada.")
    print(f"Resolución   : {width}x{height}")
    print(f"FPS salida   : {fps}")
    print(f"Frames       : {len(indexed_frames)}")
    print(f"Palette size : {palette_size}")
    print(f"Audio        : {'PCM_U8' if with_audio else 'sin audio'}")


def main():
    parser = argparse.ArgumentParser(description="Convierte .mp4 a .crptvf y opcionalmente embebe audio PCM_U8")
    parser.add_argument("input", help="Ruta del .mp4 de entrada")
    parser.add_argument("output", nargs="?", help="Ruta del .crptvf de salida")
    parser.add_argument("--width", type=int, default=128, help="Ancho de salida (default: 128)")
    parser.add_argument("--height", type=int, default=72, help="Alto de salida (default: 72)")
    parser.add_argument("--fps", type=int, default=6, help="FPS de salida (default: 6)")
    parser.add_argument("--palette-size", type=int, default=64, choices=SUPPORTED_PALETTES, help="Cantidad de colores")
    parser.add_argument("--palette-samples", type=int, default=64, help="Cantidad de frames usados para paleta global")
    parser.add_argument("--max-frames", type=int, default=None, help="Limita frames para pruebas")
    parser.add_argument("--with-audio", action="store_true", help="Embebe audio PCM_U8 mono 8k")
    parser.add_argument("--audio-sample-rate", type=int, default=8000, help="Sample rate de audio")
    parser.add_argument("--audio-channels", type=int, default=1, choices=(1,), help="Canales de audio (por ahora solo mono)")
    args = parser.parse_args()

    input_path = args.input
    output_path = args.output
    if not output_path:
        base, _ = os.path.splitext(input_path)
        output_path = base + ".crptvf"

    convert_mp4_to_crptvf(
        input_path=input_path,
        output_path=output_path,
        width=args.width,
        height=args.height,
        fps=args.fps,
        palette_size=args.palette_size,
        palette_samples=args.palette_samples,
        with_audio=args.with_audio,
        audio_sample_rate=args.audio_sample_rate,
        audio_channels=args.audio_channels,
        max_frames=args.max_frames,
    )


if __name__ == "__main__":
    main()
