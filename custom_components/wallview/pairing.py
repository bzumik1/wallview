from __future__ import annotations

import ipaddress
import logging
import secrets
import socket
import time
from dataclasses import dataclass
from http import HTTPStatus

from aiohttp import web

from homeassistant.auth.const import GROUP_ID_USER
from homeassistant.auth.models import User
from homeassistant.components import onboarding
from homeassistant.components.auth import indieauth
from homeassistant.components.http import HomeAssistantView
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EVENT_SERVICE_REGISTERED
from homeassistant.core import CALLBACK_TYPE, Event, HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError

from .const import (
    CONF_REFRESH_TOKEN_ID,
    CONF_USER_ID,
    DOMAIN,
    PAIR_URL,
    PAIRING_LIFETIME_SECONDS,
    USER_NAME,
)

_LOGGER = logging.getLogger(__name__)

ESPHOME_DOMAIN = "esphome"

PAGE = """<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WallView</title>
<style>
body {
  margin: 0;
  height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  box-sizing: border-box;
  background: #0F172A;
  color: #94A3B8;
  font: 500 17px sans-serif;
  text-align: center;
}
</style>
</head>
<body>
<div id="message">Signing in…</div>
<script>
const parameters = new URLSearchParams(location.hash.slice(1));
history.replaceState(null, "", location.pathname);
const hassUrl = location.origin;
let next = "/";
try {
  const url = new URL(parameters.get("next") || "/", hassUrl);
  if (url.origin === hassUrl) {
    next = url.href;
  }
} catch (error) {
}
try {
  const tokens = JSON.parse(parameters.get("tokens"));
  localStorage.setItem("hassTokens", JSON.stringify({
    access_token: tokens.access_token,
    token_type: tokens.token_type,
    refresh_token: tokens.refresh_token,
    expires_in: tokens.expires_in,
    hassUrl,
    clientId: `${hassUrl}/`,
    expires: Date.now() + tokens.expires_in * 1000,
  }));
  location.replace(next);
} catch (error) {
  document.getElementById("message").textContent = "Sign-in failed. Restart the kiosk.";
}
</script>
</body>
</html>
"""


def normalized_address(address: str | None) -> str:
    try:
        parsed = ipaddress.ip_address(address or "")
    except ValueError:
        return ""
    if isinstance(parsed, ipaddress.IPv6Address) and parsed.ipv4_mapped is not None:
        parsed = parsed.ipv4_mapped
    return str(parsed)


@dataclass
class PendingPairing:
    entry_id: str
    expires: float


@callback
def esphome_entry(hass: HomeAssistant, entry: ConfigEntry) -> ConfigEntry | None:
    return next(
        (other for other in hass.config_entries.async_entries(ESPHOME_DOMAIN) if other.unique_id == entry.unique_id),
        None,
    )


@callback
def service_name(hass: HomeAssistant, entry: ConfigEntry) -> str | None:
    esphome = esphome_entry(hass, entry)
    if esphome is None or not (device_name := esphome.data.get("device_name")):
        return None
    return f"{device_name.replace('-', '_')}_pair"


@callback
def async_revoke(hass: HomeAssistant, entry: ConfigEntry) -> None:
    token_id = entry.data.get(CONF_REFRESH_TOKEN_ID)
    if token_id and (token := hass.auth.async_get_refresh_token(token_id)) is not None:
        hass.auth.async_remove_refresh_token(token)


class Pairings:
    def __init__(self, hass: HomeAssistant) -> None:
        self.hass = hass
        self._pending: dict[str, PendingPairing] = {}

    def _forget(self, entry_id: str) -> None:
        now = time.monotonic()
        self._pending = {
            secret: pending
            for secret, pending in self._pending.items()
            if pending.entry_id != entry_id and pending.expires > now
        }

    async def async_send(self, entry: ConfigEntry) -> None:
        if not onboarding.async_is_onboarded(self.hass):
            raise HomeAssistantError("Home Assistant is not onboarded yet")
        if (name := service_name(self.hass, entry)) is None:
            raise HomeAssistantError("Add the panel in ESPHome first")
        self._forget(entry.entry_id)
        secret = secrets.token_urlsafe(32)
        self._pending[secret] = PendingPairing(entry.entry_id, time.monotonic() + PAIRING_LIFETIME_SECONDS)
        try:
            await self.hass.services.async_call(ESPHOME_DOMAIN, name, {"secret": secret}, blocking=True)
        except Exception:
            self._pending.pop(secret, None)
            raise

    @callback
    def async_send_when_available(self, entry: ConfigEntry) -> CALLBACK_TYPE:
        async def send() -> None:
            try:
                await self.async_send(entry)
            except HomeAssistantError as error:
                _LOGGER.warning("Could not send the sign-in to %s: %s", entry.title, error)

        if (name := service_name(self.hass, entry)) is not None and self.hass.services.has_service(
            ESPHOME_DOMAIN, name
        ):
            entry.async_create_background_task(self.hass, send(), f"{DOMAIN} pairing")
            return lambda: None

        unsubscribe: CALLBACK_TYPE | None = None

        @callback
        def registered(event: Event) -> None:
            if event.data.get("domain") != ESPHOME_DOMAIN or event.data.get("service") != service_name(
                self.hass, entry
            ):
                return
            stop()
            entry.async_create_background_task(self.hass, send(), f"{DOMAIN} pairing")

        @callback
        def stop() -> None:
            nonlocal unsubscribe
            if unsubscribe is not None:
                unsubscribe()
                unsubscribe = None

        unsubscribe = self.hass.bus.async_listen(EVENT_SERVICE_REGISTERED, registered)
        return stop

    def pending_entry(self, secret: object) -> ConfigEntry | None:
        if not isinstance(secret, str) or (pending := self._pending.get(secret)) is None:
            return None
        if pending.expires <= time.monotonic():
            self._pending.pop(secret, None)
            return None
        return self.hass.config_entries.async_get_entry(pending.entry_id)

    def consume(self, secret: str) -> bool:
        return self._pending.pop(secret, None) is not None

    async def async_panel_addresses(self, entry: ConfigEntry) -> set[str]:
        esphome = esphome_entry(self.hass, entry)
        if esphome is None or not (host := esphome.data.get("host")):
            return set()
        if address := normalized_address(host):
            return {address}
        try:
            results = await self.hass.async_add_executor_job(socket.getaddrinfo, host, None)
        except OSError:
            return set()
        return {normalized_address(str(result[4][0])) for result in results} - {""}

    async def async_get_user(self, entry: ConfigEntry) -> User:
        user_ids = [entry.data.get(CONF_USER_ID)] + [
            other.data.get(CONF_USER_ID) for other in self.hass.config_entries.async_entries(DOMAIN)
        ]
        for user_id in filter(None, user_ids):
            user = await self.hass.auth.async_get_user(user_id)
            if user is not None and user.is_active and not user.system_generated:
                return user
        return await self.hass.auth.async_create_user(USER_NAME, group_ids=[GROUP_ID_USER], local_only=True)

    async def async_issue_tokens(self, entry: ConfigEntry, client_id: str, remote_ip: str | None) -> dict:
        user = await self.async_get_user(entry)
        async_revoke(self.hass, entry)
        refresh_token = await self.hass.auth.async_create_refresh_token(
            user, client_id=client_id, client_name=entry.title
        )
        self.hass.auth.async_set_expiry(refresh_token, enable_expiry=False)
        access_token = self.hass.auth.async_create_access_token(refresh_token, remote_ip)
        self.hass.config_entries.async_update_entry(
            entry,
            data={**entry.data, CONF_USER_ID: user.id, CONF_REFRESH_TOKEN_ID: refresh_token.id},
        )
        return {
            "access_token": access_token,
            "token_type": "Bearer",
            "refresh_token": refresh_token.token,
            "expires_in": int(refresh_token.access_token_expiration.total_seconds()),
        }


class PairView(HomeAssistantView):
    url = PAIR_URL
    name = "api:wallview:pair"
    requires_auth = False

    def __init__(self, pairings: Pairings) -> None:
        self.pairings = pairings

    async def get(self, request: web.Request) -> web.Response:
        return web.Response(text=PAGE, content_type="text/html", headers={"Cache-Control": "no-store"})

    async def post(self, request: web.Request) -> web.Response:
        try:
            data = await request.json()
        except ValueError:
            return self.json_message("Invalid request", HTTPStatus.BAD_REQUEST)
        if not isinstance(data, dict):
            return self.json_message("Invalid request", HTTPStatus.BAD_REQUEST)
        client_id = data.get("client_id")
        if not isinstance(client_id, str) or not indieauth.verify_client_id(
            client_id, request_scheme=request.scheme, request_netloc=request.host
        ):
            return self.json_message("Invalid client", HTTPStatus.BAD_REQUEST)
        secret = data.get("secret")
        entry = self.pairings.pending_entry(secret)
        if entry is None:
            return self.json_message("The sign-in code is invalid or expired", HTTPStatus.FORBIDDEN)
        remote = normalized_address(request.remote)
        if not remote or remote not in await self.pairings.async_panel_addresses(entry):
            _LOGGER.warning("Sign-in code for %s used from %s, which is not the panel", entry.title, remote)
            return self.json_message("The sign-in code is invalid or expired", HTTPStatus.FORBIDDEN)
        if not self.pairings.consume(secret):
            return self.json_message("The sign-in code is invalid or expired", HTTPStatus.FORBIDDEN)
        tokens = await self.pairings.async_issue_tokens(entry, client_id, request.remote)
        _LOGGER.info("Signed in %s", entry.title)
        return self.json(tokens)
