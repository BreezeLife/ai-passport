"""Public, redaction-safe gateway errors."""

from __future__ import annotations

from typing import Any, Dict


class GatewayError(Exception):
    """An error safe to serialize to an untrusted device.

    Only the fixed public message is retained. Raw upstream bodies, request headers,
    tokens, and nested exceptions must never be placed on this exception.
    """

    def __init__(
        self,
        code: str,
        status: int,
        public_message: str,
        retryable: bool = False,
    ) -> None:
        self.code = code
        self.status = status
        self.public_message = public_message
        self.retryable = bool(retryable)
        super().__init__("%s: %s" % (code, public_message))

    def to_dict(self) -> Dict[str, Any]:
        return {
            "code": self.code,
            "message": self.public_message,
            "retryable": self.retryable,
        }
