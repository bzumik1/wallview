"""Test the WallView config flow."""

from dataclasses import replace
from unittest.mock import patch

from custom_components.wallview.const import DOMAIN
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.config_entries import SOURCE_USER, SOURCE_ZEROCONF
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .conftest import MAC, TITLE


async def test_zeroconf_confirm(
    hass: HomeAssistant, discovery_info: ZeroconfServiceInfo
) -> None:
    """Test approving a discovered panel."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": SOURCE_ZEROCONF}, data=discovery_info
    )
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "confirm"
    assert result["description_placeholders"] == {"name": TITLE}

    with patch(
        "custom_components.wallview.async_setup_entry", return_value=True
    ) as setup_entry:
        result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
        await hass.async_block_till_done()

    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["title"] == TITLE
    assert result["data"] == {}
    assert result["result"].unique_id == MAC
    assert len(setup_entry.mock_calls) == 1


async def test_zeroconf_name_from_hostname(
    hass: HomeAssistant, discovery_info: ZeroconfServiceInfo
) -> None:
    """Test the title falls back to the mDNS instance name."""
    properties = {
        key: value
        for key, value in discovery_info.properties.items()
        if key != "friendly_name"
    }
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": SOURCE_ZEROCONF},
        data=replace(discovery_info, properties=properties),
    )
    assert result["description_placeholders"] == {"name": "wallview-bbcc00"}


async def test_zeroconf_without_mac(
    hass: HomeAssistant, discovery_info: ZeroconfServiceInfo
) -> None:
    """Test a record without a MAC address is ignored."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": SOURCE_ZEROCONF},
        data=replace(
            discovery_info, properties={"project_name": "WallView.WallView OS"}
        ),
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "not_wallview"


async def test_zeroconf_already_configured(
    hass: HomeAssistant,
    discovery_info: ZeroconfServiceInfo,
    wallview_entry: MockConfigEntry,
) -> None:
    """Test a panel is only approved once."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": SOURCE_ZEROCONF}, data=discovery_info
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"


async def test_user_step_not_supported(hass: HomeAssistant) -> None:
    """Test panels cannot be added manually."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": SOURCE_USER}
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "discovery_only"
