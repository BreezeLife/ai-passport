from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from support import PACKAGE_ROOT  # noqa: F401

from workbuddy_gateway.config import GatewayConfig
from workbuddy_gateway.errors import GatewayError


class GatewayConfigTests(unittest.TestCase):
    def test_demo_mode_starts_without_external_secrets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = GatewayConfig.from_env(
                {
                    "WORKBUDDY_GATEWAY_MODE": "demo",
                    "WORKBUDDY_GATEWAY_STATE_PATH": str(Path(directory) / "state.json"),
                }
            )

        self.assertEqual("demo", config.mode)
        self.assertTrue(config.device_token)
        self.assertIsNone(config.workbuddy_access_token)
        self.assertIsNone(config.transcription_api_key)
        self.assertEqual(5.0, config.inbound_timeout_seconds)
        self.assertEqual(8, config.max_connections)

    def test_live_mode_accepts_access_token_configuration(self) -> None:
        config = GatewayConfig.from_env(self._live_env())

        self.assertEqual("https://www.workbuddy.cn", config.workbuddy_base_url)
        self.assertEqual("access-secret", config.workbuddy_access_token)

    def test_live_mode_accepts_refresh_configuration(self) -> None:
        env = self._live_env()
        del env["WORKBUDDY_ACCESS_TOKEN"]
        env.update(
            {
                "WORKBUDDY_REFRESH_TOKEN": "refresh-secret",
                "WORKBUDDY_CLIENT_ID": "client-id",
                "WORKBUDDY_CLIENT_SECRET": "client-secret",
            }
        )

        config = GatewayConfig.from_env(env)

        self.assertEqual("refresh-secret", config.workbuddy_refresh_token)

    def test_live_mode_rejects_each_missing_required_setting(self) -> None:
        required = (
            "WORKBUDDY_GATEWAY_DEVICE_TOKEN",
            "WORKBUDDY_BASE_URL",
            "WORKBUDDY_ACCESS_TOKEN",
            "WORKBUDDY_TRANSCRIPTION_URL",
            "WORKBUDDY_TRANSCRIPTION_API_KEY",
        )
        for name in required:
            with self.subTest(name=name):
                env = self._live_env()
                del env[name]
                with self.assertRaises(GatewayError) as caught:
                    GatewayConfig.from_env(env)
                self.assertEqual("invalid_configuration", caught.exception.code)

    def test_partial_refresh_configuration_is_rejected(self) -> None:
        env = self._live_env()
        del env["WORKBUDDY_ACCESS_TOKEN"]
        env["WORKBUDDY_REFRESH_TOKEN"] = "refresh-secret"

        with self.assertRaises(GatewayError):
            GatewayConfig.from_env(env)

    def test_live_urls_must_be_https(self) -> None:
        for name in ("WORKBUDDY_BASE_URL", "WORKBUDDY_TRANSCRIPTION_URL"):
            with self.subTest(name=name):
                env = self._live_env()
                env[name] = "http://example.test/service"
                with self.assertRaises(GatewayError):
                    GatewayConfig.from_env(env)

    def test_port_and_mode_are_bounded(self) -> None:
        with self.assertRaises(GatewayError):
            GatewayConfig.from_env({"WORKBUDDY_GATEWAY_MODE": "other"})
        with self.assertRaises(GatewayError):
            GatewayConfig.from_env(
                {"WORKBUDDY_GATEWAY_MODE": "demo", "WORKBUDDY_GATEWAY_PORT": "70000"}
            )

    def test_inbound_timeout_and_connection_limit_are_bounded(self) -> None:
        config = GatewayConfig.from_env(
            {
                "WORKBUDDY_GATEWAY_MODE": "demo",
                "WORKBUDDY_GATEWAY_INBOUND_TIMEOUT_SECONDS": "0.25",
                "WORKBUDDY_GATEWAY_MAX_CONNECTIONS": "2",
            }
        )
        self.assertEqual(0.25, config.inbound_timeout_seconds)
        self.assertEqual(2, config.max_connections)

        for name, value in (
            ("WORKBUDDY_REQUEST_TIMEOUT_SECONDS", "NaN"),
            ("WORKBUDDY_GATEWAY_INBOUND_TIMEOUT_SECONDS", "NaN"),
            ("WORKBUDDY_GATEWAY_INBOUND_TIMEOUT_SECONDS", "0"),
            ("WORKBUDDY_GATEWAY_INBOUND_TIMEOUT_SECONDS", "61"),
            ("WORKBUDDY_GATEWAY_MAX_CONNECTIONS", "0"),
            ("WORKBUDDY_GATEWAY_MAX_CONNECTIONS", "65"),
        ):
            with self.subTest(name=name, value=value):
                with self.assertRaises(GatewayError):
                    GatewayConfig.from_env(
                        {"WORKBUDDY_GATEWAY_MODE": "demo", name: value}
                    )

    def test_repr_and_public_summary_redact_all_tokens(self) -> None:
        env = self._live_env()
        config = GatewayConfig.from_env(env)

        rendered = repr(config) + str(config.public_summary())

        for secret in (
            env["WORKBUDDY_GATEWAY_DEVICE_TOKEN"],
            env["WORKBUDDY_ACCESS_TOKEN"],
            env["WORKBUDDY_TRANSCRIPTION_API_KEY"],
        ):
            self.assertNotIn(secret, rendered)

    @staticmethod
    def _live_env() -> dict:
        return {
            "WORKBUDDY_GATEWAY_MODE": "live",
            "WORKBUDDY_GATEWAY_DEVICE_TOKEN": "device-secret-at-least-16",
            "WORKBUDDY_BASE_URL": "https://www.workbuddy.cn",
            "WORKBUDDY_ACCESS_TOKEN": "access-secret",
            "WORKBUDDY_TRANSCRIPTION_URL": "https://speech.example/v1/audio/transcriptions",
            "WORKBUDDY_TRANSCRIPTION_API_KEY": "speech-secret",
        }


if __name__ == "__main__":
    unittest.main()
