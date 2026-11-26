"""Test delivering and redeeming kiosk sign-in codes."""

from collections.abc import Awaitable, Callable
from http import HTTPStatus
import socket
from typing import Any
from unittest.mock import patch

from aiohttp.test_utils import TestClient
from custom_components.wallview.const import (
    CONF_REFRESH_TOKEN_ID,
    CONF_USER_ID,
    EVENT_SIGN_IN_REQUESTED,
    PAIR_URL,
)
from custom_components.wallview.pairing import DATA_PAIRINGS, normalized_address
import pytest
from pytest_homeassistant_custom_component.common import (
    MockConfigEntry,
    async_mock_service,
)

from homeassistant.config_entries import ConfigEntryDisabler
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr
from homeassistant.setup import async_setup_component

from .conftest import MAC, PAIR_ACTION, TITLE

type ClientFactory = Callable[[], Awaitable[TestClient]]


async def _setup(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    """Set up the WallView entry."""
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()


async def _redeem(client: TestClient, secret: str) -> Any:
    """Redeem a sign-in code as the panel's start page does."""
    return await client.post(
        PAIR_URL, json={"secret": secret, "client_id": str(client.make_url("/"))}
    )


@pytest.fixture
def pair_calls(hass: HomeAssistant) -> list[ServiceCall]:
    """Register the panel's ESPHome pairing action."""
    return async_mock_service(hass, "esphome", PAIR_ACTION)


@pytest.mark.usefixtures("esphome_entry")
async def test_first_code_sent_when_action_appears(
    hass: HomeAssistant, wallview_entry: MockConfigEntry
) -> None:
    """Test the first code is sent once ESPHome registers the pairing action."""
    await _setup(hass, wallview_entry)
    hass.services.async_register("esphome", "unrelated", lambda call: None)
    await hass.async_block_till_done()
    calls = async_mock_service(hass, "esphome", PAIR_ACTION)
    await hass.async_block_till_done()

    assert len(calls) == 1
    assert len(calls[0].data["secret"]) >= 32


@pytest.mark.usefixtures("esphome_entry")
async def test_redeem_code(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test the panel exchanges its code for tokens of the kiosk user."""
    await _setup(hass, wallview_entry)
    assert len(pair_calls) == 1
    secret = pair_calls[0].data["secret"]
    client = await hass_client_no_auth()

    response = await _redeem(client, secret)
    assert response.status == HTTPStatus.OK
    tokens = await response.json()
    assert tokens["token_type"] == "Bearer"
    assert tokens["expires_in"] > 0

    refresh_token = hass.auth.async_get_refresh_token(
        wallview_entry.data[CONF_REFRESH_TOKEN_ID]
    )
    assert refresh_token is not None
    assert refresh_token.token == tokens["refresh_token"]
    assert refresh_token.client_name == TITLE
    assert refresh_token.expire_at is not None
    user = refresh_token.user
    assert user.id == wallview_entry.data[CONF_USER_ID]
    assert user.name == "WallView"
    assert user.local_only
    assert not user.is_admin

    response = await _redeem(client, secret)
    assert response.status == HTTPStatus.FORBIDDEN


@pytest.mark.usefixtures("esphome_entry")
async def test_pairing_again_replaces_token(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test a second sign-in revokes the first token and reuses the user."""
    await _setup(hass, wallview_entry)
    client = await hass_client_no_auth()
    assert (await _redeem(client, pair_calls[0].data["secret"])).status == HTTPStatus.OK
    first_token_id = wallview_entry.data[CONF_REFRESH_TOKEN_ID]
    user_id = wallview_entry.data[CONF_USER_ID]

    await hass.data[DATA_PAIRINGS].async_send(wallview_entry)
    assert (await _redeem(client, pair_calls[1].data["secret"])).status == HTTPStatus.OK

    assert hass.auth.async_get_refresh_token(first_token_id) is None
    assert wallview_entry.data[CONF_REFRESH_TOKEN_ID] != first_token_id
    assert wallview_entry.data[CONF_USER_ID] == user_id


@pytest.mark.usefixtures("esphome_entry")
async def test_new_code_invalidates_old_code(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test only the newest code of a panel can be redeemed."""
    await _setup(hass, wallview_entry)
    await hass.data[DATA_PAIRINGS].async_send(wallview_entry)
    client = await hass_client_no_auth()

    response = await _redeem(client, pair_calls[0].data["secret"])
    assert response.status == HTTPStatus.FORBIDDEN
    response = await _redeem(client, pair_calls[1].data["secret"])
    assert response.status == HTTPStatus.OK


async def test_code_rejected_from_other_address(
    hass: HomeAssistant,
    esphome_entry: MockConfigEntry,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test a code is only accepted from the panel and stays valid otherwise."""
    hass.config_entries.async_update_entry(
        esphome_entry, data={**esphome_entry.data, "host": "192.0.2.1"}
    )
    await _setup(hass, wallview_entry)
    secret = pair_calls[0].data["secret"]
    client = await hass_client_no_auth()

    response = await _redeem(client, secret)
    assert response.status == HTTPStatus.FORBIDDEN
    assert CONF_REFRESH_TOKEN_ID not in wallview_entry.data
    assert hass.data[DATA_PAIRINGS].pending_entry(secret) is wallview_entry


async def test_code_rejected_after_esphome_removed(
    hass: HomeAssistant,
    esphome_entry: MockConfigEntry,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test a code cannot be redeemed when the panel's address is unknown."""
    await _setup(hass, wallview_entry)
    secret = pair_calls[0].data["secret"]
    hass.config_entries.async_update_entry(esphome_entry, data={})
    client = await hass_client_no_auth()
    assert (await _redeem(client, secret)).status == HTTPStatus.FORBIDDEN


@pytest.mark.usefixtures("esphome_entry")
async def test_expired_code(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test an expired code is rejected."""
    await _setup(hass, wallview_entry)
    client = await hass_client_no_auth()
    with patch("custom_components.wallview.pairing.time.monotonic", return_value=1e12):
        response = await _redeem(client, pair_calls[0].data["secret"])
    assert response.status == HTTPStatus.FORBIDDEN


@pytest.mark.usefixtures("esphome_entry")
async def test_code_redeemed_concurrently(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test a code consumed while its request was checked is rejected."""
    await _setup(hass, wallview_entry)
    client = await hass_client_no_auth()
    with patch.object(hass.data[DATA_PAIRINGS], "consume", return_value=False):
        response = await _redeem(client, pair_calls[0].data["secret"])
    assert response.status == HTTPStatus.FORBIDDEN


@pytest.mark.usefixtures("esphome_entry")
@pytest.mark.parametrize(
    ("body", "status"),
    [
        ("not json", HTTPStatus.BAD_REQUEST),
        ("[]", HTTPStatus.BAD_REQUEST),
        ('{"secret": "x"}', HTTPStatus.BAD_REQUEST),
        (
            '{"secret": "x", "client_id": "https://other.example/"}',
            HTTPStatus.BAD_REQUEST,
        ),
        ('{"secret": 1, "client_id": "CLIENT"}', HTTPStatus.FORBIDDEN),
        ('{"secret": "unknown", "client_id": "CLIENT"}', HTTPStatus.FORBIDDEN),
    ],
)
async def test_invalid_requests(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    hass_client_no_auth: ClientFactory,
    body: str,
    status: HTTPStatus,
) -> None:
    """Test malformed or unknown requests are rejected."""
    await _setup(hass, wallview_entry)
    client = await hass_client_no_auth()
    response = await client.post(
        PAIR_URL,
        data=body.replace("CLIENT", str(client.make_url("/"))),
        headers={"Content-Type": "application/json"},
    )
    assert response.status == status


@pytest.mark.usefixtures("esphome_entry")
async def test_token_page(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    hass_client_no_auth: ClientFactory,
) -> None:
    """Test the page that stores the tokens in the frontend."""
    await _setup(hass, wallview_entry)
    client = await hass_client_no_auth()
    response = await client.get(PAIR_URL)
    assert response.status == HTTPStatus.OK
    assert response.headers["Cache-Control"] == "no-store"
    assert "hassTokens" in await response.text()


@pytest.mark.usefixtures("esphome_entry")
async def test_sign_in_requested_event(
    hass: HomeAssistant,
    esphome_entry: MockConfigEntry,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    device_registry: dr.DeviceRegistry,
) -> None:
    """Test a sign-in request from the panel's ESPHome device sends a new code."""
    await _setup(hass, wallview_entry)
    panel = device_registry.async_get_or_create(
        config_entry_id=esphome_entry.entry_id,
        connections={(dr.CONNECTION_NETWORK_MAC, MAC)},
        identifiers={("esphome", "panel")},
    )
    other_entry = MockConfigEntry(domain="other")
    other_entry.add_to_hass(hass)
    other = device_registry.async_get_or_create(
        config_entry_id=other_entry.entry_id,
        connections={(dr.CONNECTION_NETWORK_MAC, MAC)},
    )
    stranger = device_registry.async_get_or_create(
        config_entry_id=other_entry.entry_id,
        connections={(dr.CONNECTION_NETWORK_MAC, "02:00:00:00:00:99")},
        identifiers={("other", "stranger")},
    )
    no_mac = device_registry.async_get_or_create(
        config_entry_id=other_entry.entry_id,
        connections={(dr.CONNECTION_UPNP, "uuid")},
        identifiers={("other", "no_mac")},
    )

    for device_id in (other.id, stranger.id, no_mac.id, "unknown", None, 1):
        hass.bus.async_fire(EVENT_SIGN_IN_REQUESTED, {"device_id": device_id})
        await hass.async_block_till_done()
    assert len(pair_calls) == 1

    hass.bus.async_fire(EVENT_SIGN_IN_REQUESTED, {"device_id": panel.id})
    await hass.async_block_till_done()
    assert len(pair_calls) == 2


@pytest.mark.usefixtures("esphome_entry")
async def test_sign_in_request_for_disabled_entry(
    hass: HomeAssistant,
    esphome_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    device_registry: dr.DeviceRegistry,
) -> None:
    """Test a panel whose sign-in is disabled gets no code."""
    entry = MockConfigEntry(
        domain="wallview",
        unique_id=MAC,
        version=2,
        disabled_by=ConfigEntryDisabler.USER,
    )
    entry.add_to_hass(hass)
    assert await async_setup_component(hass, "wallview", {})
    panel = device_registry.async_get_or_create(
        config_entry_id=esphome_entry.entry_id,
        connections={(dr.CONNECTION_NETWORK_MAC, MAC)},
    )
    hass.bus.async_fire(EVENT_SIGN_IN_REQUESTED, {"device_id": panel.id})
    await hass.async_block_till_done()
    assert pair_calls == []


@pytest.mark.usefixtures("esphome_entry")
async def test_not_onboarded(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    pair_calls: list[ServiceCall],
    caplog: pytest.LogCaptureFixture,
) -> None:
    """Test no code is sent before onboarding is finished."""
    with patch(
        "custom_components.wallview.pairing.onboarding.async_is_onboarded",
        return_value=False,
    ):
        await _setup(hass, wallview_entry)
    assert pair_calls == []
    assert "Could not send the sign-in" in caplog.text


async def test_send_without_esphome(
    hass: HomeAssistant, wallview_entry: MockConfigEntry
) -> None:
    """Test sending fails with a translated error without the ESPHome entry."""
    await _setup(hass, wallview_entry)
    with pytest.raises(HomeAssistantError) as error:
        await hass.data[DATA_PAIRINGS].async_send(wallview_entry)
    assert error.value.translation_key == "esphome_missing"
    assert error.value.translation_placeholders == {"name": TITLE}


@pytest.mark.usefixtures("esphome_entry")
async def test_pairing_action_fails(
    hass: HomeAssistant,
    wallview_entry: MockConfigEntry,
    caplog: pytest.LogCaptureFixture,
) -> None:
    """Test a failing pairing action leaves no valid code behind."""

    secrets: list[str] = []

    def fail(call: ServiceCall) -> None:
        secrets.append(call.data["secret"])
        raise HomeAssistantError("panel offline")

    hass.services.async_register("esphome", PAIR_ACTION, fail)
    await _setup(hass, wallview_entry)
    assert "panel offline" in caplog.text
    assert len(secrets) == 1
    assert hass.data[DATA_PAIRINGS].pending_entry(secrets[0]) is None


async def test_panel_address_from_hostname(
    hass: HomeAssistant,
    esphome_entry: MockConfigEntry,
    wallview_entry: MockConfigEntry,
) -> None:
    """Test a host name in the ESPHome entry is resolved."""
    hass.config_entries.async_update_entry(
        esphome_entry, data={**esphome_entry.data, "host": "panel.local"}
    )
    await _setup(hass, wallview_entry)
    pairings = hass.data[DATA_PAIRINGS]
    result = [(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("192.0.2.5", 0))]
    with patch(
        "custom_components.wallview.pairing.socket.getaddrinfo", return_value=result
    ):
        assert await pairings.async_panel_addresses(wallview_entry) == {"192.0.2.5"}
    with patch(
        "custom_components.wallview.pairing.socket.getaddrinfo", side_effect=OSError
    ):
        assert await pairings.async_panel_addresses(wallview_entry) == set()


@pytest.mark.parametrize(
    ("address", "expected"),
    [
        ("192.0.2.1", "192.0.2.1"),
        ("::ffff:192.0.2.1", "192.0.2.1"),
        ("2001:db8::1", "2001:db8::1"),
        ("panel.local", ""),
        (None, ""),
    ],
)
def test_normalized_address(address: str | None, expected: str) -> None:
    """Test IP addresses are normalized."""
    assert normalized_address(address) == expected
