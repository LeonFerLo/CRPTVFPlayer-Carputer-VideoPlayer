from flask import Flask, request, Response, send_file, abort
from pathlib import Path
import subprocess
import sys
import uuid
import threading

app = Flask(__name__)

# =========================
# CONFIG
# =========================
SERVER_USER = "admin"
SERVER_PASS = "1234"

BASE_DIR = Path(__file__).resolve().parent
VIDEOS_MP4 = BASE_DIR / "videos_mp4"
VIDEOS_CRPTVF = BASE_DIR / "videos_crptvf"
CONVERTER_SCRIPT = BASE_DIR / "mp4_to_crptvf.py"

# Ajustá estos valores si querés permitir otras opciones
ALLOWED_FPS = {8, 10, 12, 15}
ALLOWED_PALETTES = {8, 16, 64, 128, 256}
ALLOWED_RESOLUTIONS = {
    (240, 101),  # pantalla completa
    (160, 67),   # media
    (120, 50),   # pequeña
}

# job_id -> Path del archivo generado
GENERATED_FILES = {}


# =========================
# HELPERS
# =========================
def is_auth_ok(user: str, password: str) -> bool:
    return user == SERVER_USER and password == SERVER_PASS


def safe_mp4_list(root: Path):
    if not root.exists():
        return []

    items = []
    for file in sorted(root.rglob("*.mp4")):
        if file.is_file():
            rel = file.relative_to(root).as_posix()
            # La ruta que recibe el Cardputer para volver a mandar al server
            remote_path = rel
            display_name = file.name
            items.append((display_name, remote_path))
    return items


def resolve_mp4_path(remote_path: str) -> Path:
    if not remote_path:
        raise ValueError("ruta vacia")

    # Normalizar
    remote_path = remote_path.strip().replace("\\", "/").lstrip("/")

    full = (VIDEOS_MP4 / remote_path).resolve()
    video_root_resolved = VIDEOS_MP4.resolve()

    if not str(full).startswith(str(video_root_resolved)):
        raise ValueError("path traversal detectado")

    return full


def schedule_delete(path: Path, delay_seconds: float = 1.0):
    def _delete():
        try:
            if path.exists():
                path.unlink()
        except Exception as e:
            print(f"[WARN] no se pudo borrar {path}: {e}")

    timer = threading.Timer(delay_seconds, _delete)
    timer.daemon = True
    timer.start()


# =========================
# API: LISTADO MP4
# =========================
@app.post("/api/list_videos")
def list_videos():
    user = request.form.get("user", "")
    password = request.form.get("pass", "")

    print("POST /api/list_videos")
    print("remote addr =", request.remote_addr)
    print("user =", user)

    if not is_auth_ok(user, password):
        return Response("unauthorized\n", status=401, mimetype="text/plain")

    videos = safe_mp4_list(VIDEOS_MP4)

    lines = []
    for name, remote_path in videos:
        lines.append(f"{name}|{remote_path}")

    return Response("\n".join(lines), mimetype="text/plain; charset=utf-8")


# =========================
# API: PREPARAR CONVERSION
# =========================
@app.post("/api/prepare_video")
def prepare_video():
    user = request.form.get("user", "")
    password = request.form.get("pass", "")
    video = request.form.get("video", "").strip()

    print("POST /api/prepare_video")
    print("remote addr =", request.remote_addr)
    print("video =", video)

    if not is_auth_ok(user, password):
        return Response("unauthorized\n", status=401, mimetype="text/plain")

    try:
        fps = int(request.form.get("fps", "8"))
        palette = int(request.form.get("palette", "64"))
        audio = int(request.form.get("audio", "1"))
        width = int(request.form.get("width", "160"))
        height = int(request.form.get("height", "67"))
    except ValueError:
        return Response("invalid numeric params\n", status=400, mimetype="text/plain")

    if fps not in ALLOWED_FPS:
        return Response("invalid fps\n", status=400, mimetype="text/plain")

    if palette not in ALLOWED_PALETTES:
        return Response("invalid palette\n", status=400, mimetype="text/plain")

    if audio not in {0, 1}:
        return Response("invalid audio\n", status=400, mimetype="text/plain")

    if (width, height) not in ALLOWED_RESOLUTIONS:
        return Response("invalid resolution\n", status=400, mimetype="text/plain")

    try:
        input_file = resolve_mp4_path(video)
    except ValueError:
        return Response("invalid video path\n", status=400, mimetype="text/plain")

    if not input_file.exists() or not input_file.is_file():
        return Response("video not found\n", status=404, mimetype="text/plain")

    if not CONVERTER_SCRIPT.exists():
        return Response("converter not found\n", status=500, mimetype="text/plain")

    VIDEOS_CRPTVF.mkdir(parents=True, exist_ok=True)

    job_id = uuid.uuid4().hex[:16]
    output_name = f"{input_file.stem}_{width}x{height}_{fps}fps_{palette}.crptvf"
    output_file = VIDEOS_CRPTVF / f"{job_id}_{output_name}"

    # Adaptado para el conversor que ya venías usando.
    # Ajustá estos argumentos si tu script usa otra interfaz.
    cmd = [
        sys.executable,
        str(CONVERTER_SCRIPT),
        str(input_file),
        str(output_file),
        "--width", str(width),
        "--height", str(height),
        "--fps", str(fps),
        "--palette-size", str(palette),
    ]

    if audio == 1:
        cmd.append("--with-audio")

    print("CMD =", " ".join(cmd))

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=600,
            cwd=str(BASE_DIR),
        )
    except subprocess.TimeoutExpired:
        return Response("conversion timeout\n", status=500, mimetype="text/plain")
    except Exception as e:
        return Response(f"conversion exception: {e}\n", status=500, mimetype="text/plain")

    if result.returncode != 0:
        msg = result.stderr.strip() or result.stdout.strip() or "conversion failed"
        print("[ERROR] converter stderr:", result.stderr)
        print("[ERROR] converter stdout:", result.stdout)
        return Response((msg[:500] + "\n"), status=500, mimetype="text/plain")

    if not output_file.exists():
        print("[ERROR] el conversor terminó OK pero no apareció:", output_file)
        return Response("output missing\n", status=500, mimetype="text/plain")

    GENERATED_FILES[job_id] = output_file

    return Response(
        f"ok|{job_id}|{output_file.name}|/api/download/{job_id}",
        mimetype="text/plain"
    )


# =========================
# API: DESCARGA DE ARCHIVO GENERADO
# =========================
@app.get("/api/download/<job_id>")
def download_generated(job_id):
    output_file = GENERATED_FILES.get(job_id)
    if not output_file:
        abort(404)

    if not output_file.exists() or not output_file.is_file():
        GENERATED_FILES.pop(job_id, None)
        abort(404)

    response = send_file(
        output_file,
        as_attachment=True,
        download_name=output_file.name,
        mimetype="application/octet-stream"
    )

    # quitar del diccionario y borrar poco después
    GENERATED_FILES.pop(job_id, None)
    schedule_delete(output_file, delay_seconds=1.0)

    return response


# =========================
# MAIN
# =========================
if __name__ == "__main__":
    VIDEOS_MP4.mkdir(parents=True, exist_ok=True)
    VIDEOS_CRPTVF.mkdir(parents=True, exist_ok=True)

    app.run(
        host="0.0.0.0",
        port=8080,
        debug=True
    )
