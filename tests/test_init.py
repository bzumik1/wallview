"""Test setting up, migrating and removing WallView config entries."""

from custom_components.wallview.const import (
    CONF_REFRESH_TOKEN_ID,
    CONF_USER_ID,
    DOMAIN,
    ISSUE_ESPHOME_MISSING,
)
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.auth.const import GROUP_ID_USER
from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant
from homeassistant.helpers import (
    device_registry as dr,
    entity_registry as er,
    issue_registry as ir,
)

from .conftest import MAC, TITLE


async def test_setup_and_unload(
    hass: HomeAssistant,
    esphome_entry: MockConfigEntry,
    wallview_entry: MockConfigEntry,
    issue_registry: ir.IssueRegistry,
) -> None:
    """Test an entry loads and unloads without a repair issue."""
    assert await hass.config_entries.async_setup(wallview_entry.entry_id)
    await hass.async_block_till_done()
    assert wallview_entry.state is ConfigEntryState.LOADED
    assert len(issue_registry.issues) == 0

    assert await hass.config_entries.async_unload(wallview_entry.entry_id)
    assert wallview_entry.state is ConfigEntryState.NOT_LOADED


async def test_repair_issue_without_esphome(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    issue_registry: ir.IssueRegistry,
) -> None:
    """Test a repair issue asks to add the panel in ESPHome."""
    assert await hass.config_entries.async_setup(wallview_entry.entry_id)
    await hass.async_block_till_done()

    issue = issue_registry.async_get_issue(
        DOMAIN, f"{ISSUE_ESPHOME_MISSING}_{wallview_entry.entry_id}"
    )
    assert issue is not None
    assert issue.translation_placeholders == {"name": TITLE}

    await hass.config_entries.async_remove(wallview_entry.entry_id)
    assert len(issue_registry.issues) == 0


async def test_paired_entry_sends_nothing(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    issue_registry: ir.IssueRegistry,
) -> None:
    """Test an already signed-in panel gets no new code and no issue on setup."""
    hass.config_entries.async_update_entry(
        wallview_entry, data={CONF_REFRESH_TOKEN_ID: "token"}
    )
    assert await hass.config_entries.async_setup(wallview_entry.entry_id)
    await hass.async_block_till_done()
    assert len(issue_registry.issues) == 0


async def test_migrate_from_version_1(
    hass: HomeAssistant,
    device_registry: dr.DeviceRegistry,
    entity_registry: er.EntityRegistry,
    esphome_entry: MockConfigEntry,
) -> None:
    """Test the button and the second device of version 1 are removed."""
    entry = MockConfigEntry(domain=DOMAIN, unique_id=MAC, title=TITLE, version=1)
    entry.add_to_hass(hass)
    device = device_registry.async_get_or_create(
        config_entry_id=entry.entry_id, connections={(dr.CONNECTION_NETWORK_MAC, MAC)}
    )
    entity_registry.async_get_or_create(
        "button",
        DOMAIN,
        f"{MAC}_sign_in_kiosk",
        config_entry=entry,
        device_id=device.id,
    )

    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()

    assert entry.version == 2
    assert entry.state is ConfigEntryState.LOADED
    assert dr.async_entries_for_config_entry(device_registry, entry.entry_id) == []
    assert er.async_entries_for_config_entry(entity_registry, entry.entry_id) == []


async def test_migrate_from_future_version(hass: HomeAssistant) -> None:
    """Test entries from a newer version are not loaded."""
    entry = MockConfigEntry(domain=DOMAIN, unique_id=MAC, title=TITLE, version=3)
    entry.add_to_hass(hass)
    assert not await hass.config_entries.async_setup(entry.entry_id)
    assert entry.state is ConfigEntryState.MIGRATION_ERROR


async def test_remove_last_entry_removes_user(hass: HomeAssistant) -> None:
    """Test removing the last panel revokes its token and removes the kiosk user."""
    user = await hass.auth.async_create_user(
        "WallView", group_ids=[GROUP_ID_USER], local_only=True
    )
    token = await hass.auth.async_create_refresh_token(
        user, client_id="http://example.local/"
    )
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=MAC,
        title=TITLE,
        version=2,
        data={CONF_USER_ID: user.id, CONF_REFRESH_TOKEN_ID: token.id},
    )
    entry.add_to_hass(hass)

    await hass.config_entries.async_remove(entry.entry_id)

    assert hass.auth.async_get_refresh_token(token.id) is None
    assert await hass.auth.async_get_user(user.id) is None


async def test_remove_entry_keeps_shared_user(hass: HomeAssistant) -> None:
    """Test the kiosk user stays while another panel uses it."""
    user = await hass.auth.async_create_user(
        "WallView", group_ids=[GROUP_ID_USER], local_only=True
    )
    token = await hass.auth.async_create_refresh_token(
        user, client_id="http://example.local/"
    )
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=MAC,
        version=2,
        data={CONF_USER_ID: user.id, CONF_REFRESH_TOKEN_ID: token.id},
    )
    entry.add_to_hass(hass)
    MockConfigEntry(
        domain=DOMAIN,
        unique_id="02:00:aa:bb:cc:01",
        version=2,
        data={CONF_USER_ID: user.id},
    ).add_to_hass(hass)

    await hass.config_entries.async_remove(entry.entry_id)

    assert hass.auth.async_get_refresh_token(token.id) is None
    assert await hass.auth.async_get_user(user.id) is not None


async def test_remove_unpaired_entry(
    hass: HomeAssistant, wallview_entry: MockConfigEntry
) -> None:
    """Test removing a panel that never signed in."""
    await hass.config_entries.async_remove(wallview_entry.entry_id)
    assert hass.config_entries.async_entries(DOMAIN) == []
