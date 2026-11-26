from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.typing import ConfigType

from .const import CONF_REFRESH_TOKEN_ID, DOMAIN
from .pairing import Pairings, PairView

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)
PLATFORMS = [Platform.BUTTON]


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    pairings = Pairings(hass)
    hass.data[DOMAIN] = pairings
    hass.http.register_view(PairView(pairings))
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    if CONF_REFRESH_TOKEN_ID not in entry.data:
        entry.async_on_unload(hass.data[DOMAIN].async_send_when_available(entry))
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)


async def async_remove_entry(hass: HomeAssistant, entry: ConfigEntry) -> None:
    hass.data[DOMAIN].async_revoke(entry)
