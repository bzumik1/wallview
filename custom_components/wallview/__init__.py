"""The WallView integration: automatic kiosk sign-in for WallView panels."""

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import (
    config_validation as cv,
    device_registry as dr,
    entity_registry as er,
)
from homeassistant.helpers.typing import ConfigType

from .const import DOMAIN, EVENT_SIGN_IN_REQUESTED
from .pairing import (
    DATA_PAIRINGS,
    Pairings,
    PairView,
    async_remove_issue,
    async_remove_kiosk_credentials,
)

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

type WallViewConfigEntry = ConfigEntry[None]


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    """Set up the sign-in endpoint and the sign-in request listener."""
    pairings = Pairings(hass)
    hass.data[DATA_PAIRINGS] = pairings
    hass.http.register_view(PairView(pairings))
    hass.bus.async_listen(
        EVENT_SIGN_IN_REQUESTED, pairings.async_handle_sign_in_request
    )
    return True


async def async_setup_entry(hass: HomeAssistant, entry: WallViewConfigEntry) -> bool:
    """Set up kiosk sign-in for an approved panel."""
    entry.async_on_unload(hass.data[DATA_PAIRINGS].async_setup_entry(entry))
    return True


async def async_unload_entry(hass: HomeAssistant, entry: WallViewConfigEntry) -> bool:
    """Unload a config entry."""
    return True


async def async_remove_entry(hass: HomeAssistant, entry: WallViewConfigEntry) -> None:
    """Revoke the kiosk credentials of a removed panel."""
    async_remove_issue(hass, entry)
    await async_remove_kiosk_credentials(hass, entry)


async def async_migrate_entry(hass: HomeAssistant, entry: WallViewConfigEntry) -> bool:
    """Migrate old config entries."""
    if entry.version == 1:
        entity_registry = er.async_get(hass)
        for entity in er.async_entries_for_config_entry(
            entity_registry, entry.entry_id
        ):
            entity_registry.async_remove(entity.entity_id)
        device_registry = dr.async_get(hass)
        for device in dr.async_entries_for_config_entry(
            device_registry, entry.entry_id
        ):
            device_registry.async_remove_device(device.id)
        hass.config_entries.async_update_entry(entry, version=2)
    return True
