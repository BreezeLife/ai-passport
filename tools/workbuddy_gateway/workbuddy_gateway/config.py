"""Validated environment configuration with redaction-safe representations."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional
from urllib.parse import urlsplit

from .errors import GatewayError


def _config_error(message: str) -> GatewayError:
    return GatewayError("invalid_configuration", 500, message, False)


def _parse_port(value: str) -> int:
    try:
        port = int(value)
    except (TypeError, ValueError):
        raise _config_error("Gateway port must be an integer")
    if port < 0 or port > 65535:
        raise _config_error("Gateway port is out of range")
    return port


def _parse_timeout(value: str) -> float:
    try:
        timeout = float(value)
    except (TypeError, ValueError):
        raise _config_error("Request timeout must be numeric")
    if timeout <= 0 or timeout > 120:
        raise _config_error("Request timeout is out of range")
    return timeout


def _validate_https(value: Optional[str], label: str) -> str:
    if not value:
        raise _config_error("Missing %s" % label)
    parsed = urlsplit(value)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise _config_error("%s must be an HTTPS URL without embedded credentials" % label)
    return value.rstrip("/")


@dataclass(frozen=True, repr=False)
class GatewayConfig:
    mode: str
    host: str
    port: int
    device_token: str
    state_path: Path
    workbuddy_base_url: Optional[str] = None
    workbuddy_access_token: Optional[str] = None
    workbuddy_refresh_token: Optional[str] = None
    workbuddy_client_id: Optional[str] = None
    workbuddy_client_secret: Optional[str] = None
    transcription_url: Optional[str] = None
    transcription_api_key: Optional[str] = None
    transcription_model: str = "gpt-4o-mini-transcribe"
    request_timeout_seconds: float = 15.0

    @classmethod
    def from_env(cls, environ: Optional[Mapping[str, str]] = None) -> "GatewayConfig":
        values = os.environ if environ is None else environ
        mode = values.get("WORKBUDDY_GATEWAY_MODE", "demo").strip().lower()
        if mode not in ("demo", "live"):
            raise _config_error("Gateway mode must be demo or live")
        host = values.get("WORKBUDDY_GATEWAY_HOST", "127.0.0.1").strip()
        if not host:
            raise _config_error("Gateway host is required")
        port = _parse_port(values.get("WORKBUDDY_GATEWAY_PORT", "8787"))
        state_path = Path(values.get("WORKBUDDY_GATEWAY_STATE_PATH", ".workbuddy-gateway-state.json"))
        timeout = _parse_timeout(values.get("WORKBUDDY_REQUEST_TIMEOUT_SECONDS", "15"))

        if mode == "demo":
            return cls(
                mode=mode,
                host=host,
                port=port,
                device_token=values.get(
                    "WORKBUDDY_GATEWAY_DEVICE_TOKEN", "workbuddy-demo-device-token"
                ),
                state_path=state_path,
                request_timeout_seconds=timeout,
            )

        device_token = values.get("WORKBUDDY_GATEWAY_DEVICE_TOKEN", "")
        if len(device_token) < 16:
            raise _config_error("Missing or weak gateway device token")
        workbuddy_url = _validate_https(values.get("WORKBUDDY_BASE_URL"), "WorkBuddy base URL")
        transcription_url = _validate_https(
            values.get("WORKBUDDY_TRANSCRIPTION_URL"), "transcription URL"
        )
        access_token = values.get("WORKBUDDY_ACCESS_TOKEN") or None
        refresh_token = values.get("WORKBUDDY_REFRESH_TOKEN") or None
        client_id = values.get("WORKBUDDY_CLIENT_ID") or None
        client_secret = values.get("WORKBUDDY_CLIENT_SECRET") or None
        has_refresh_set = all((refresh_token, client_id, client_secret))
        has_partial_refresh = any((refresh_token, client_id, client_secret)) and not has_refresh_set
        if has_partial_refresh or (not access_token and not has_refresh_set):
            raise _config_error(
                "WorkBuddy access token or complete refresh credentials are required"
            )
        transcription_key = values.get("WORKBUDDY_TRANSCRIPTION_API_KEY") or None
        if not transcription_key:
            raise _config_error("Missing transcription API key")

        return cls(
            mode=mode,
            host=host,
            port=port,
            device_token=device_token,
            state_path=state_path,
            workbuddy_base_url=workbuddy_url,
            workbuddy_access_token=access_token,
            workbuddy_refresh_token=refresh_token,
            workbuddy_client_id=client_id,
            workbuddy_client_secret=client_secret,
            transcription_url=transcription_url,
            transcription_api_key=transcription_key,
            transcription_model=values.get(
                "WORKBUDDY_TRANSCRIPTION_MODEL", "gpt-4o-mini-transcribe"
            ),
            request_timeout_seconds=timeout,
        )

    def public_summary(self) -> Dict[str, Any]:
        return {
            "mode": self.mode,
            "host": self.host,
            "port": self.port,
            "state_path": str(self.state_path),
            "workbuddy_configured": bool(
                self.workbuddy_access_token or self.workbuddy_refresh_token
            ),
            "transcription_configured": bool(self.transcription_api_key),
        }

    def __repr__(self) -> str:
        return "GatewayConfig(%r)" % self.public_summary()
