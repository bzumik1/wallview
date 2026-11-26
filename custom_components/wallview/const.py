from __future__ import annotations

from typing import TYPE_CHECKING

from homeassistant.util.hass_dict import HassKey

if TYPE_CHECKING:
    from .pairing import Pairings

DOMAIN = "wallview"
DATA_PAIRINGS: HassKey[Pairings] = HassKey(DOMAIN)
EVENT_SIGN_IN_REQUESTED = "esphome.wallview_sign_in_requested"

CONF_USER_ID = "user_id"
CONF_REFRESH_TOKEN_ID = "refresh_token_id"

USER_NAME = "WallView"
PAIRING_LIFETIME_SECONDS = 600
PAIR_URL = "/api/wallview/pair"
