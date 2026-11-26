"""Kiosk sign-in for WallView panels."""

from dataclasses import dataclass
from http import HTTPStatus
import ipaddress
import logging
import secrets
import socket
import time
from typing import TypedDict, TypeIs
from urllib.parse import urlsplit

from aiohttp import web

from homeassistant.auth.const import GROUP_ID_USER
from homeassistant.auth.models import User
from homeassistant.components import onboarding
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EVENT_SERVICE_REGISTERED
from homeassistant.core import CALLBACK_TYPE, Event, HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr, issue_registry as ir
from homeassistant.helpers.http import HomeAssistantView
from homeassistant.util.hass_dict import HassKey

from .const import (
    CONF_REFRESH_TOKEN_ID,
    CONF_USER_ID,
    DOMAIN,
    ESPHOME_DOMAIN,
    ISSUE_ESPHOME_MISSING,
    PAIR_URL,
    PAIRING_LIFETIME_SECONDS,
    USER_NAME,
)

_LOGGER = logging.getLogger(__name__)

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


class TokenResponse(TypedDict):
    """Tokens handed to the kiosk start page."""

    access_token: str
    token_type: str
    refresh_token: str
    expires_in: int


@dataclass
class PendingPairing:
    """A one-time sign-in code waiting to be redeemed by a panel."""

    entry_id: str
    expires: float


def normalized_address(address: str | None) -> str:
    """Return an IP address in canonical form, IPv4-mapped IPv6 as IPv4."""
    try:
        parsed = ipaddress.ip_address(address or "")
    except ValueError:
        return ""
    if isinstance(parsed, ipaddress.IPv6Address) and parsed.ipv4_mapped is not None:
        parsed = parsed.ipv4_mapped
    return str(parsed)


def valid_client_id(client_id: object, request: web.Request) -> TypeIs[str]:
    """Return whether the client ID is this Home Assistant's own frontend origin."""
    if not isinstance(client_id, str):
        return False
    parts = urlsplit(client_id)
    return (
        parts.scheme in ("http", "https")
        and parts.path == "/"
        and not parts.query
        and not parts.fragment
        and parts.netloc.lower() == request.host.lower()
    )


@callback
def esphome_entry(hass: HomeAssistant, entry: ConfigEntry) -> ConfigEntry | None:
    """Return the ESPHome config entry of the same panel."""
    return next(
        (
            other
            for other in hass.config_entries.async_entries(ESPHOME_DOMAIN)
            if other.unique_id == entry.unique_id
        ),
        None,
    )


@callback
def service_name(hass: HomeAssistant, entry: ConfigEntry) -> str | None:
    """Return the name of the panel's ESPHome pairing action."""
    esphome = esphome_entry(hass, entry)
    if esphome is None or not (device_name := esphome.data.get("device_name")):
        return None
    return f"{device_name.replace('-', '_')}_pair"


def _issue_id(entry: ConfigEntry) -> str:
    """Return the repair issue ID of a panel."""
    return f"{ISSUE_ESPHOME_MISSING}_{entry.entry_id}"


@callback
def async_create_issue(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Ask the user to add the panel in ESPHome."""
    ir.async_create_issue(
        hass,
        DOMAIN,
        _issue_id(entry),
        is_fixable=False,
        severity=ir.IssueSeverity.WARNING,
        translation_key=ISSUE_ESPHOME_MISSING,
        translation_placeholders={"name": entry.title},
    )


@callback
def async_remove_issue(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Remove the repair issue of a panel."""
    ir.async_delete_issue(hass, DOMAIN, _issue_id(entry))


@callback
def async_revoke(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Revoke the refresh token issued to a panel."""
    token_id = entry.data.get(CONF_REFRESH_TOKEN_ID)
    if token_id and (token := hass.auth.async_get_refresh_token(token_id)) is not None:
        hass.auth.async_remove_refresh_token(token)


async def async_remove_kiosk_credentials(
    hass: HomeAssistant, entry: ConfigEntry
) -> None:
    """Revoke a panel's token and remove the kiosk user with the last panel."""
    async_revoke(hass, entry)
    if not (user_id := entry.data.get(CONF_USER_ID)):
        return
    if any(
        other.data.get(CONF_USER_ID) == user_id
        for other in hass.config_entries.async_entries(DOMAIN)
        if other.entry_id != entry.entry_id
    ):
        return
    user = await hass.auth.async_get_user(user_id)
    if user is not None and not user.system_generated and not user.is_owner:
        await hass.auth.async_remove_user(user)


class Pairings:
    """Create, deliver and redeem one-time sign-in codes."""

    def __init__(self, hass: HomeAssistant) -> None:
        """Initialize the pairing state."""
        self.hass = hass
        self._pending: dict[str, PendingPairing] = {}

    def _forget(self, entry_id: str) -> None:
        """Drop expired codes and all codes of a panel."""
        now = time.monotonic()
        self._pending = {
            secret: pending
            for secret, pending in self._pending.items()
            if pending.entry_id != entry_id and pending.expires > now
        }

    @callback
    def async_setup_entry(self, entry: ConfigEntry) -> CALLBACK_TYPE:
        """Send the first sign-in code once the panel is available in ESPHome."""
        if CONF_REFRESH_TOKEN_ID in entry.data:
            return lambda: None
        if esphome_entry(self.hass, entry) is None:
            async_create_issue(self.hass, entry)
        return self.async_send_when_available(entry)

    async def async_send(self, entry: ConfigEntry) -> None:
        """Create a sign-in code and send it to the panel."""
        if not onboarding.async_is_onboarded(self.hass):
            raise HomeAssistantError(
                translation_domain=DOMAIN, translation_key="not_onboarded"
            )
        if (name := service_name(self.hass, entry)) is None:
            raise HomeAssistantError(
                translation_domain=DOMAIN,
                translation_key=ISSUE_ESPHOME_MISSING,
                translation_placeholders={"name": entry.title},
            )
        self._forget(entry.entry_id)
        secret = secrets.token_urlsafe(32)
        self._pending[secret] = PendingPairing(
            entry.entry_id, time.monotonic() + PAIRING_LIFETIME_SECONDS
        )
        try:
            await self.hass.services.async_call(
                ESPHOME_DOMAIN, name, {"secret": secret}, blocking=True
            )
        except Exception:
            self._pending.pop(secret, None)
            raise
        async_remove_issue(self.hass, entry)

    async def _async_try_send(self, entry: ConfigEntry) -> None:
        """Send a sign-in code and log instead of raising."""
        try:
            await self.async_send(entry)
        except HomeAssistantError as error:
            _LOGGER.warning("Could not send the sign-in to %s: %s", entry.title, error)

    @callback
    def async_send_when_available(self, entry: ConfigEntry) -> CALLBACK_TYPE:
        """Send a sign-in code as soon as the panel's pairing action exists."""
        if (
            name := service_name(self.hass, entry)
        ) is not None and self.hass.services.has_service(ESPHOME_DOMAIN, name):
            entry.async_create_background_task(
                self.hass, self._async_try_send(entry), f"{DOMAIN} pairing"
            )
            return lambda: None

        unsubscribe: CALLBACK_TYPE | None = None

        @callback
        def registered(event: Event) -> None:
            if event.data.get("domain") != ESPHOME_DOMAIN or event.data.get(
                "service"
            ) != service_name(self.hass, entry):
                return
            stop()
            entry.async_create_background_task(
                self.hass, self._async_try_send(entry), f"{DOMAIN} pairing"
            )

        @callback
        def stop() -> None:
            nonlocal unsubscribe
            if unsubscribe is not None:
                unsubscribe()
                unsubscribe = None

        unsubscribe = self.hass.bus.async_listen(EVENT_SERVICE_REGISTERED, registered)
        return stop

    @callback
    def entry_for_device(self, device_id: object) -> ConfigEntry | None:
        """Return the approved WallView entry of an ESPHome device."""
        if not isinstance(device_id, str):
            return None
        device_registry = dr.async_get(self.hass)
        device = device_registry.async_get(device_id)
        if not isinstance(device, dr.DeviceEntry):
            return None
        for connection_type, mac in device.connections:
            if connection_type != dr.CONNECTION_NETWORK_MAC:
                continue
            entry = self.hass.config_entries.async_entry_for_domain_unique_id(
                DOMAIN, dr.format_mac(mac)
            )
            if entry is None or entry.disabled_by is not None:
                continue
            esphome = esphome_entry(self.hass, entry)
            if esphome is not None and device in dr.async_entries_for_config_entry(
                device_registry, esphome.entry_id
            ):
                return entry
        return None

    async def async_handle_sign_in_request(self, event: Event) -> None:
        """Answer a panel's sign-in request with a new code."""
        if (entry := self.entry_for_device(event.data.get("device_id"))) is not None:
            await self._async_try_send(entry)

    def pending_entry(self, secret: object) -> ConfigEntry | None:
        """Return the entry a valid code belongs to."""
        if (
            not isinstance(secret, str)
            or (pending := self._pending.get(secret)) is None
        ):
            return None
        if pending.expires <= time.monotonic():
            self._pending.pop(secret, None)
            return None
        return self.hass.config_entries.async_get_entry(pending.entry_id)

    def consume(self, secret: str) -> bool:
        """Invalidate a code, return whether it was still valid."""
        return self._pending.pop(secret, None) is not None

    async def async_panel_addresses(self, entry: ConfigEntry) -> set[str]:
        """Return the IP addresses of the panel as known to ESPHome."""
        esphome = esphome_entry(self.hass, entry)
        if esphome is None or not (host := esphome.data.get("host")):
            return set()
        if address := normalized_address(host):
            return {address}
        try:
            results = await self.hass.async_add_executor_job(
                socket.getaddrinfo, host, None
            )
        except OSError:
            return set()
        return {normalized_address(str(result[4][0])) for result in results} - {""}

    async def async_get_user(self, entry: ConfigEntry) -> User:
        """Return the kiosk user, creating it on first use."""
        user_ids = [entry.data.get(CONF_USER_ID)] + [
            other.data.get(CONF_USER_ID)
            for other in self.hass.config_entries.async_entries(DOMAIN)
        ]
        for user_id in filter(None, user_ids):
            user = await self.hass.auth.async_get_user(user_id)
            if user is not None and user.is_active and not user.system_generated:
                return user
        return await self.hass.auth.async_create_user(
            USER_NAME, group_ids=[GROUP_ID_USER], local_only=True
        )

    async def async_issue_tokens(
        self, entry: ConfigEntry, client_id: str, remote_ip: str | None
    ) -> TokenResponse:
        """Issue tokens for the kiosk, replacing the panel's previous token."""
        user = await self.async_get_user(entry)
        async_revoke(self.hass, entry)
        refresh_token = await self.hass.auth.async_create_refresh_token(
            user, client_id=client_id, client_name=entry.title
        )
        access_token = self.hass.auth.async_create_access_token(
            refresh_token, remote_ip
        )
        self.hass.config_entries.async_update_entry(
            entry,
            data={
                **entry.data,
                CONF_USER_ID: user.id,
                CONF_REFRESH_TOKEN_ID: refresh_token.id,
            },
        )
        return TokenResponse(
            access_token=access_token,
            token_type="Bearer",
            refresh_token=refresh_token.token,
            expires_in=int(refresh_token.access_token_expiration.total_seconds()),
        )


DATA_PAIRINGS: HassKey[Pairings] = HassKey(DOMAIN)


class PairView(HomeAssistantView):
    """Endpoint where a panel redeems its sign-in code."""

    url = PAIR_URL
    name = "api:wallview:pair"
    requires_auth = False

    def __init__(self, pairings: Pairings) -> None:
        """Initialize the view."""
        self.pairings = pairings

    async def get(self, request: web.Request) -> web.Response:
        """Serve the page that stores the tokens in the frontend."""
        return web.Response(
            text=PAGE, content_type="text/html", headers={"Cache-Control": "no-store"}
        )

    async def post(self, request: web.Request) -> web.Response:
        """Exchange a sign-in code from the panel's own address for tokens."""
        try:
            data = await request.json()
        except ValueError:
            return self.json_message("Invalid request", HTTPStatus.BAD_REQUEST)
        if not isinstance(data, dict):
            return self.json_message("Invalid request", HTTPStatus.BAD_REQUEST)
        client_id = data.get("client_id")
        if not valid_client_id(client_id, request):
            return self.json_message("Invalid client", HTTPStatus.BAD_REQUEST)
        secret = data.get("secret")
        entry = self.pairings.pending_entry(secret)
        if entry is None or not isinstance(secret, str):
            return self.json_message(
                "The sign-in code is invalid or expired", HTTPStatus.FORBIDDEN
            )
        remote = normalized_address(request.remote)
        if not remote or remote not in await self.pairings.async_panel_addresses(entry):
            _LOGGER.warning(
                "Sign-in code for %s used from %s, which is not the panel",
                entry.title,
                remote,
            )
            return self.json_message(
                "The sign-in code is invalid or expired", HTTPStatus.FORBIDDEN
            )
        if not self.pairings.consume(secret):
            return self.json_message(
                "The sign-in code is invalid or expired", HTTPStatus.FORBIDDEN
            )
        tokens = await self.pairings.async_issue_tokens(
            entry, client_id, request.remote
        )
        _LOGGER.info("Signed in %s", entry.title)
        return self.json(tokens)
