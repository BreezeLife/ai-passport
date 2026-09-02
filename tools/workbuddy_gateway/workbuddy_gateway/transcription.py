"""Bounded PCM/WAV conversion and transcription-provider adapter."""

from __future__ import annotations

import io
import json
import secrets
import socket
import urllib.error
import urllib.request
import wave
from typing import Any
from urllib.parse import urlsplit

from .errors import GatewayError
from .models import MAX_TRANSCRIPT_BYTES, truncate_utf8


SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2
MIN_PCM_BYTES = SAMPLE_RATE * SAMPLE_WIDTH
MAX_PCM_BYTES = SAMPLE_RATE * SAMPLE_WIDTH * 5
MAX_PROVIDER_RESPONSE_BYTES = 16 * 1024


def _validate_pcm(pcm: bytes) -> bytes:
    if not isinstance(pcm, bytes):
        raise GatewayError("invalid_audio", 400, "Audio must be raw PCM bytes", False)
    if len(pcm) < MIN_PCM_BYTES or len(pcm) > MAX_PCM_BYTES or len(pcm) % SAMPLE_WIDTH:
        raise GatewayError(
            "invalid_audio",
            400,
            "Audio must contain one to five seconds of 16 kHz mono PCM",
            False,
        )
    return pcm


def pcm_to_wav(pcm: bytes) -> bytes:
    clean_pcm = _validate_pcm(pcm)
    output = io.BytesIO()
    with wave.open(output, "wb") as writer:
        writer.setnchannels(CHANNELS)
        writer.setsampwidth(SAMPLE_WIDTH)
        writer.setframerate(SAMPLE_RATE)
        writer.writeframes(clean_pcm)
    return output.getvalue()


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args: Any, **kwargs: Any) -> None:
        del args, kwargs
        return None


class TranscriptionClient:
    def __init__(
        self,
        url: str,
        api_key: str,
        model: str,
        *,
        timeout_seconds: float = 30.0,
        max_response_bytes: int = MAX_PROVIDER_RESPONSE_BYTES,
        allow_insecure_http: bool = False,
    ) -> None:
        parsed = urlsplit(url)
        allowed_http = allow_insecure_http and parsed.scheme == "http"
        if (
            parsed.scheme != "https"
            and not allowed_http
            or not parsed.hostname
            or parsed.username
            or parsed.password
        ):
            raise GatewayError(
                "invalid_configuration",
                500,
                "Transcription URL must be HTTPS without embedded credentials",
                False,
            )
        if not api_key or not model:
            raise GatewayError(
                "invalid_configuration", 500, "Transcription credentials are incomplete", False
            )
        if timeout_seconds <= 0 or max_response_bytes < 256:
            raise GatewayError(
                "invalid_configuration", 500, "Transcription client limits are invalid", False
            )
        self._url = url
        self._api_key = api_key
        self._model = model
        self._timeout_seconds = timeout_seconds
        self._max_response_bytes = max_response_bytes
        self._opener = urllib.request.build_opener(_NoRedirectHandler())

    def transcribe(self, pcm: bytes) -> str:
        wav_bytes = pcm_to_wav(pcm)
        boundary = "wb-" + secrets.token_hex(16)
        body = self._multipart(boundary, wav_bytes)
        request = urllib.request.Request(
            self._url,
            data=body,
            method="POST",
            headers={
                "Authorization": "Bearer " + self._api_key,
                "Content-Type": "multipart/form-data; boundary=" + boundary,
                "Accept": "application/json",
            },
        )
        try:
            response = self._opener.open(request, timeout=self._timeout_seconds)
            with response:
                raw = response.read(self._max_response_bytes + 1)
        except urllib.error.HTTPError as error:
            error.close()
            raise GatewayError(
                "transcription_failed", 502, "Transcription provider request failed", True
            )
        except (urllib.error.URLError, socket.timeout, TimeoutError):
            raise GatewayError(
                "transcription_failed", 502, "Transcription provider request failed", True
            )
        if len(raw) > self._max_response_bytes:
            raise GatewayError(
                "transcription_schema", 502, "Transcription response was too large", True
            )
        try:
            payload = json.loads(raw.decode("utf-8", "strict"))
        except (UnicodeError, ValueError):
            raise GatewayError(
                "transcription_schema", 502, "Transcription provider returned invalid JSON", True
            )
        if not isinstance(payload, dict) or not isinstance(payload.get("text"), str):
            raise GatewayError(
                "transcription_schema", 502, "Transcription provider returned invalid text", True
            )
        text = payload["text"].strip()
        if not text:
            raise GatewayError(
                "transcription_schema", 502, "Transcription provider returned no text", True
            )
        return truncate_utf8(text, MAX_TRANSCRIPT_BYTES)

    def _multipart(self, boundary: str, wav_bytes: bytes) -> bytes:
        marker = boundary.encode("ascii")
        model = self._model.encode("utf-8", "strict")
        return b"".join(
            (
                b"--" + marker + b'\r\nContent-Disposition: form-data; name="model"\r\n\r\n',
                model,
                b"\r\n--"
                + marker
                + b'\r\nContent-Disposition: form-data; name="file"; filename="audio.wav"\r\n',
                b"Content-Type: audio/wav\r\n\r\n",
                wav_bytes,
                b"\r\n--" + marker + b"--\r\n",
            )
        )

    def __repr__(self) -> str:
        return "TranscriptionClient(url=%r, model=%r, api_key=<redacted>)" % (
            self._url,
            self._model,
        )


class DemoTranscriber:
    def transcribe(self, pcm: bytes) -> str:
        _validate_pcm(pcm)
        return "Please prepare a short project update."
