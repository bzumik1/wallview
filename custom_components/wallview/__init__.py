from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.typing import ConfigType

from .const import CONF_REFRESH_TOKEN_ID, DATA_PAIRINGS, DOMAIN, EVENT_SIGN_IN_REQUESTED
from .pairing import Pairings, PairView, async_revoke

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    pairings = Pairings(hass)
    hass.data[DATA_PAIRINGS] = pairings
    hass.http.register_view(PairView(pairings))
    hass.bus.async_listen(EVENT_SIGN_IN_REQUESTED, pairings.async_handle_sign_in_request)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    if CONF_REFRESH_TOKEN_ID not in entry.data:
        entry.async_on_unload(hass.data[DATA_PAIRINGS].async_send_when_available(entry))
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    return True


async def async_remove_entry(hass: HomeAssistant, entry: ConfigEntry) -> None:
    async_revoke(hass, entry)


async def async_migrate_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    if entry.version > 2:
        return False
    if entry.version == 1:
        entity_registry = er.async_get(hass)
        for entity in er.async_entries_for_config_entry(entity_registry, entry.entry_id):
            entity_registry.async_remove(entity.entity_id)
        device_registry = dr.async_get(hass)
        for device in dr.async_entries_for_config_entry(device_registry, entry.entry_id):
            device_registry.async_remove_device(device.id)
        hass.config_entries.async_update_entry(entry, version=2)
    return True
