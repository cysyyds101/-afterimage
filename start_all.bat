@echo off
setlocal
title Whale-Maid LAN one-click launcher
echo ============================================================
echo   Whale-Maid LAN stack: platform + speech + bridge + cutout
echo   (board connects directly via ws://10.1.41.11:8780/voice)
echo ============================================================
echo.

REM 1) character-cloud platform (vinext dev, port 5173)
start "character-cloud :5173" cmd /k "cd /d D:\work\character-cloud && npm run dev"

REM 2) speech-service ASR(SenseVoice)+TTS(Edge-TTS), port 8782.
REM    Required by voice-bridge; without it voice cannot transcribe/speak.
start "speech-service :8782" cmd /k "cd /d D:\work\whale-maid\speech-service && .venv\Scripts\python.exe app.py"

REM 3) voice-bridge WebSocket voice service, port 8780 (start.ps1 loads .env)
start "voice-bridge :8780" cmd /k "cd /d D:\work\whale-maid\voice-bridge && powershell -NoProfile -ExecutionPolicy Bypass -File start.ps1"

REM 4) BiRefNet cutout microservice, port 8781
start "BiRefNet cutout :8781" cmd /k "cd /d D:\work\character-cloud\services\birefnet && powershell -NoProfile -ExecutionPolicy Bypass -File run-local.ps1 -Port 8781"

echo.
echo Launched 4 windows:
echo   :5173 character-cloud   :8782 speech-service   :8780 voice-bridge   :8781 BiRefNet cutout
echo.
echo No tunnel needed: the board joins the same 10.1.41.x Wi-Fi as the PC.
echo Close each window to stop its service. This launcher window can be closed.
endlocal