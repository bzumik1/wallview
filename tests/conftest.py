"""Fixtures for the WallView integration tests."""

from ipaddress import ip_address

from custom_components.wallview.const import DOMAIN
import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry, MockUser

from homeassistant.core import HomeAssistant
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

MAC = "02:00:aa:bb:cc:00"
DEVICE_NAME = "wallview-bbcc00"
PAIR_ACTION = "wallview_bbcc00_pair"
TITLE = "WallView CC00"


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations: None) -> None:
    """Enable loading the WallView custom integration."""


@pytest.fixture(autouse=True)
def onboarded_owner(hass_owner_user: MockUser) -> MockUser:
    """Create the owner, as onboarding does before any panel is approved."""
    return hass_owner_user


@pytest.fixture
def discovery_info() -> ZeroconfServiceInfo:
    """Return the mDNS record a WallView panel advertises."""
    return ZeroconfServiceInfo(
        ip_address=ip_address("127.0.0.1"),
        ip_addresses=[ip_address("127.0.0.1")],
        hostname=f"{DEVICE_NAME}.local.",
        name=f"{DEVICE_NAME}._esphomelib._tcp.local.",
        port=6053,
        type="_esphomelib._tcp.local.",
        properties={
            "mac": MAC.replace(":", ""),
            "friendly_name": TITLE,
            "project_name": "WallView.WallView OS",
        },
    )


@pytest.fixture
def esphome_entry(hass: HomeAssistant) -> MockConfigEntry:
    """Add the panel's ESPHome config entry without setting it up."""
    entry = MockConfigEntry(
        domain="esphome",
        unique_id=MAC,
        data={"host": "127.0.0.1", "port": 6053, "device_name": DEVICE_NAME},
    )
    entry.add_to_hass(hass)
    return entry


@pytest.fixture
def wallview_entry(hass: HomeAssistant) -> MockConfigEntry:
    """Add an approved WallView config entry."""
    entry = MockConfigEntry(domain=DOMAIN, unique_id=MAC, title=TITLE, version=2)
    entry.add_to_hass(hass)
    return entry
