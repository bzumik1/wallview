from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import CONNECTION_NETWORK_MAC, DeviceInfo
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .const import DOMAIN


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([SignInButton(entry)])


class SignInButton(ButtonEntity):
    _attr_has_entity_name = True
    _attr_translation_key = "sign_in_kiosk"
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, entry: ConfigEntry) -> None:
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_sign_in_kiosk"
        self._attr_device_info = DeviceInfo(connections={(CONNECTION_NETWORK_MAC, entry.unique_id)})

    async def async_press(self) -> None:
        await self.hass.data[DOMAIN].async_send(self._entry)
