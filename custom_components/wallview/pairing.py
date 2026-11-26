from __future__ import annotations

import logging
import secrets
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
    CONF_NODE,
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
(async () => {
  const query = new URLSearchParams(location.search);
  const hassUrl = location.origin;
  const clientId = `${hassUrl}/`;
  let next = "/";
  try {
    const url = new URL(query.get("next") || "/", hassUrl);
    if (url.origin === hassUrl) {
      next = url.href;
    }
  } catch (error) {
  }
  history.replaceState(null, "", location.pathname);
  try {
    const response = await fetch(location.pathname, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ secret: query.get("secret"), client_id: clientId }),
    });
    const tokens = await response.json();
    if (!response.ok) {
      throw new Error(tokens.message || `error ${response.status}`);
    }
    localStorage.setItem("hassTokens", JSON.stringify({
      ...tokens,
      hassUrl,
      clientId,
      expires: Date.now() + tokens.expires_in * 1000,
    }));
    location.replace(next);
  } catch (error) {
    document.getElementById("message").textContent =
      `Sign-in failed: ${error.message}. Press "Sign in kiosk" for this panel in Home Assistant.`;
  }
})();
</script>
</body>
</html>
"""


@dataclass
class PendingPairing:
    entry_id: str
    expires: float


def service_name(entry: ConfigEntry) -> str:
    return f"{entry.data[CONF_NODE].replace('-', '_')}_pair"


class Pairings:
    def __init__(self, hass: HomeAssistant) -> None:
        self.hass = hass
        self._pending: dict[str, PendingPairing] = {}

    async def async_send(self, entry: ConfigEntry) -> None:
        if not onboarding.async_is_onboarded(self.hass):
            raise HomeAssistantError("Home Assistant is not onboarded yet")
        secret = secrets.token_urlsafe(32)
        self._pending[secret] = PendingPairing(entry.entry_id, time.monotonic() + PAIRING_LIFETIME_SECONDS)
        await self.hass.services.async_call(ESPHOME_DOMAIN, service_name(entry), {"secret": secret}, blocking=True)

    @callback
    def async_send_when_available(self, entry: ConfigEntry) -> CALLBACK_TYPE:
        name = service_name(entry)

        async def send() -> None:
            try:
                await self.async_send(entry)
            except HomeAssistantError as error:
                _LOGGER.warning("Could not send the sign-in to %s: %s", entry.title, error)

        @callback
        def registered(event: Event) -> None:
            if event.data.get("domain") == ESPHOME_DOMAIN and event.data.get("service") == name:
                unsubscribe()
                entry.async_create_background_task(self.hass, send(), f"{DOMAIN} pairing")

        if self.hass.services.has_service(ESPHOME_DOMAIN, name):
            entry.async_create_background_task(self.hass, send(), f"{DOMAIN} pairing")
            return lambda: None
        unsubscribe = self.hass.bus.async_listen(EVENT_SERVICE_REGISTERED, registered)
        return unsubscribe

    def consume(self, secret: object) -> ConfigEntry | None:
        now = time.monotonic()
        self._pending = {key: value for key, value in self._pending.items() if value.expires > now}
        if not isinstance(secret, str) or (pending := self._pending.pop(secret, None)) is None:
            return None
        return self.hass.config_entries.async_get_entry(pending.entry_id)

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
        self.async_revoke(entry)
        refresh_token = await self.hass.auth.async_create_refresh_token(
            user, client_id=client_id, client_name=entry.title
        )
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

    @callback
    def async_revoke(self, entry: ConfigEntry) -> None:
        token_id = entry.data.get(CONF_REFRESH_TOKEN_ID)
        if token_id and (token := self.hass.auth.async_get_refresh_token(token_id)) is not None:
            self.hass.auth.async_remove_refresh_token(token)


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
        entry = self.pairings.consume(data.get("secret"))
        if entry is None:
            return self.json_message("The sign-in code is invalid or expired", HTTPStatus.FORBIDDEN)
        tokens = await self.pairings.async_issue_tokens(entry, client_id, request.remote)
        _LOGGER.info("Signed in %s", entry.title)
        return self.json(tokens)
