from __future__ import annotations

import io
import json
import unittest
import wave

from support import PACKAGE_ROOT, RecordingServer  # noqa: F401

from workbuddy_gateway.errors import GatewayError
from workbuddy_gateway.models import MAX_TRANSCRIPT_BYTES
from workbuddy_gateway.transcription import (
    MAX_PCM_BYTES,
    MIN_PCM_BYTES,
    DemoTranscriber,
    TranscriptionClient,
    pcm_to_wav,
)


class TranscriptionTests(unittest.TestCase):
    def test_one_second_pcm_becomes_exact_mono_16khz_16bit_wav(self) -> None:
        pcm = b"\x01\x02" * 16000

        wav_bytes = pcm_to_wav(pcm)

        with wave.open(io.BytesIO(wav_bytes), "rb") as reader:
            self.assertEqual(1, reader.getnchannels())
            self.assertEqual(2, reader.getsampwidth())
            self.assertEqual(16000, reader.getframerate())
            self.assertEqual(16000, reader.getnframes())
            self.assertEqual(pcm, reader.readframes(16000))
        self.assertEqual(len(pcm) + 44, len(wav_bytes))

    def test_five_seconds_is_accepted_but_short_long_or_odd_pcm_is_rejected(self) -> None:
        self.assertTrue(pcm_to_wav(b"\x00" * MAX_PCM_BYTES).startswith(b"RIFF"))
        for pcm in (
            b"\x00" * (MIN_PCM_BYTES - 2),
            b"\x00" * (MAX_PCM_BYTES + 2),
            b"\x00" * (MIN_PCM_BYTES + 1),
        ):
            with self.subTest(length=len(pcm)):
                with self.assertRaises(GatewayError) as caught:
                    pcm_to_wav(pcm)
                self.assertEqual("invalid_audio", caught.exception.code)

    def test_client_sends_bounded_multipart_without_exposing_api_key(self) -> None:
        api_key = "speech-key-must-not-leak"
        with RecordingServer() as provider:
            def transcribe(request):
                self.assertEqual("Bearer " + api_key, request.headers["authorization"])
                content_type = request.headers["content-type"]
                self.assertTrue(content_type.startswith("multipart/form-data; boundary="))
                self.assertIn(b'Content-Disposition: form-data; name="model"', request.body)
                self.assertIn(b"gpt-test-model", request.body)
                self.assertIn(b'filename="audio.wav"', request.body)
                self.assertIn(b"RIFF", request.body)
                return 200, {"Content-Type": "application/json"}, json.dumps({"text": "你好"}).encode()

            provider.route("POST", "/v1/audio/transcriptions", transcribe)
            client = TranscriptionClient(
                provider.base_url + "/v1/audio/transcriptions",
                api_key,
                "gpt-test-model",
                allow_insecure_http=True,
            )

            result = client.transcribe(b"\x00\x00" * 16000)

        self.assertEqual("你好", result)
        self.assertNotIn(api_key, repr(client))

    def test_provider_text_is_utf8_bounded(self) -> None:
        with RecordingServer() as provider:
            provider.json_route(
                "POST",
                "/v1/audio/transcriptions",
                {"text": "字" * 1000},
            )
            client = TranscriptionClient(
                provider.base_url + "/v1/audio/transcriptions",
                "key",
                "model",
                allow_insecure_http=True,
            )
            result = client.transcribe(b"\x00\x00" * 16000)

        self.assertLessEqual(len(result.encode("utf-8")), MAX_TRANSCRIPT_BYTES)

    def test_provider_timeout_error_and_malformed_json_are_public_and_redacted(self) -> None:
        api_key = "provider-secret"
        with RecordingServer() as provider:
            provider.route(
                "POST",
                "/v1/audio/transcriptions",
                lambda _request: (
                    500,
                    {"Content-Type": "application/json"},
                    json.dumps({"error": api_key}).encode(),
                ),
            )
            client = TranscriptionClient(
                provider.base_url + "/v1/audio/transcriptions",
                api_key,
                "model",
                allow_insecure_http=True,
            )
            with self.assertRaises(GatewayError) as caught:
                client.transcribe(b"\x00\x00" * 16000)

        self.assertEqual("transcription_failed", caught.exception.code)
        self.assertNotIn(api_key, str(caught.exception))

        with RecordingServer() as provider:
            provider.route(
                "POST",
                "/v1/audio/transcriptions",
                lambda _request: (200, {"Content-Type": "application/json"}, b"not-json"),
            )
            client = TranscriptionClient(
                provider.base_url + "/v1/audio/transcriptions",
                "key",
                "model",
                allow_insecure_http=True,
            )
            with self.assertRaises(GatewayError) as malformed:
                client.transcribe(b"\x00\x00" * 16000)
        self.assertEqual("transcription_schema", malformed.exception.code)

    def test_demo_transcriber_is_deterministic_and_enforces_audio_bounds(self) -> None:
        transcriber = DemoTranscriber()
        pcm = b"\x00\x00" * 16000

        self.assertEqual(transcriber.transcribe(pcm), transcriber.transcribe(pcm))
        with self.assertRaises(GatewayError):
            transcriber.transcribe(b"")


if __name__ == "__main__":
    unittest.main()
