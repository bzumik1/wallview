"""Constants for the WallView integration."""

from typing import Final

DOMAIN: Final = "wallview"
ESPHOME_DOMAIN: Final = "esphome"

EVENT_SIGN_IN_REQUESTED: Final = "esphome.wallview_sign_in_requested"

CONF_USER_ID: Final = "user_id"
CONF_REFRESH_TOKEN_ID: Final = "refresh_token_id"

USER_NAME: Final = "WallView"
PAIRING_LIFETIME_SECONDS: Final = 600
PAIR_URL: Final = "/api/wallview/pair"

ISSUE_ESPHOME_MISSING: Final = "esphome_missing"
