"""Config flow for the WallView integration."""

from typing import Any

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.helpers.device_registry import format_mac
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import DOMAIN


class WallViewConfigFlow(ConfigFlow, domain=DOMAIN):
    """Approve kiosk sign-in for a discovered WallView panel."""

    VERSION = 2

    def __init__(self) -> None:
        """Initialize the flow."""
        self._name = ""

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """Handle a panel discovered through its ESPHome mDNS record."""
        mac = discovery_info.properties.get("mac")
        if not mac:
            return self.async_abort(reason="not_wallview")
        self._name = (
            discovery_info.properties.get("friendly_name")
            or discovery_info.name.split(".", 1)[0]
        )
        await self.async_set_unique_id(format_mac(mac))
        self._abort_if_unique_id_configured()
        self.context["title_placeholders"] = {"name": self._name}
        return await self.async_step_confirm()

    async def async_step_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Ask the user to approve the kiosk sign-in."""
        if user_input is not None:
            return self.async_create_entry(title=self._name, data={})
        self._set_confirm_only()
        return self.async_show_form(
            step_id="confirm", description_placeholders={"name": self._name}
        )

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Explain that panels are only added through discovery."""
        return self.async_abort(reason="discovery_only")
