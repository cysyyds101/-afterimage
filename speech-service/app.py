"""Local OpenAI-compatible speech shim for the whale-maid voice bridge.

ASR: FunASR SenseVoiceSmall (CPU, key-free, ModelScope weights).
TTS: Edge-TTS zh-CN-XiaoxiaoNeural (key-free), decoded to 16-bit PCM.

Exposes /v1/audio/transcriptions and /v1/audio/speech so the Node bridge can
use it exactly like a hosted OpenAI speech endpoint. Local-only; no auth.
"""
import io
import os
import re
import tempfile
import wave
from contextlib import asynccontextmanager

import numpy as np
import soundfile as sf
import edge_tts
import uvicorn
from fastapi import FastAPI, File, Form, Request, UploadFile
from fastapi.responses import JSONResponse, Response

PORT = int(os.environ.get("SPEECH_PORT", "8782"))
ASR_MODEL = os.environ.get("ASR_MODEL", "iic/SenseVoiceSmall")
DEFAULT_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
TAG_RE = re.compile(r"<\|[^|]*\|>")

state = {"asr": None, "asr_error": None}


def load_asr():
    from funasr import AutoModel
    state["asr"] = AutoModel(model=ASR_MODEL, trust_remote_code=True, device="cpu", disable_update=True)


@asynccontextmanager
async def lifespan(app: FastAPI):
    try:
        load_asr()
    except Exception as exc:  # noqa: BLE001
        state["asr_error"] = str(exc)
        print("ASR load failed:", exc, flush=True)
    yield


app = FastAPI(lifespan=lifespan)


@app.get("/health")
def health():
    return {"ok": True, "asrReady": state["asr"] is not None, "asrModel": ASR_MODEL,
            "asrError": state["asr_error"], "ttsVoice": DEFAULT_VOICE}


def transcribe_bytes(data: bytes) -> str:
    if state["asr"] is None:
        raise RuntimeError(f"ASR not ready: {state['asr_error']}")
    # Transient temp file only; removed immediately so no raw audio is persisted.
    fd, path = tempfile.mkstemp(suffix=".wav")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        result = state["asr"].generate(input=path, cache={}, language="zh", use_itn=True, batch_size_s=60)
        text = ""
        if result and isinstance(result, list):
            text = result[0].get("text", "")
        return TAG_RE.sub("", text).strip()
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


@app.post("/v1/audio/transcriptions")
@app.post("/audio/transcriptions")
async def transcriptions(file: UploadFile = File(...), model: str = Form("")):
    data = await file.read()
    try:
        text = transcribe_bytes(data)
    except Exception as exc:  # noqa: BLE001
        return JSONResponse({"error": {"message": str(exc)}}, status_code=503)
    return {"text": text}


def mp3_to_pcm16(mp3: bytes):
    audio, rate = sf.read(io.BytesIO(mp3), dtype="float32", always_2d=True)
    mono = audio.mean(axis=1)
    pcm = np.clip(mono * 32767.0, -32768, 32767).astype("<i2")
    return pcm.tobytes(), int(rate)


def pcm_to_wav(pcm: bytes, rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(pcm)
    return buf.getvalue()


@app.post("/v1/audio/speech")
@app.post("/audio/speech")
async def speech(request: Request):
    body = await request.json()
    text = str(body.get("input") or "").strip()
    voice = str(body.get("voice") or DEFAULT_VOICE)
    fmt = str(body.get("response_format") or "pcm")
    if not text:
        return JSONResponse({"error": {"message": "input is empty"}}, status_code=400)
    try:
        communicate = edge_tts.Communicate(text, voice)
        chunks = []
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                chunks.append(chunk["data"])
        mp3 = b"".join(chunks)
        if not mp3:
            return JSONResponse({"error": {"message": "Edge-TTS returned no audio"}}, status_code=502)
        pcm, rate = mp3_to_pcm16(mp3)
    except Exception as exc:  # noqa: BLE001
        return JSONResponse({"error": {"message": f"TTS failed: {exc}"}}, status_code=502)
    if fmt == "wav":
        return Response(content=pcm_to_wav(pcm, rate), media_type="audio/wav")
    return Response(content=pcm, media_type="application/octet-stream",
                    headers={"X-Sample-Rate": str(rate)})


if __name__ == "__main__":
    uvicorn.run(app, host="127.0.0.1", port=PORT, log_level="info")