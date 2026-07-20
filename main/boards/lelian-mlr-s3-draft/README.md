# LeLian MLR S3 draft board port

This is a draft board profile based on the user's board photo. It is intentionally
conservative because the photo shows net labels such as `LCD_DIO`, `LCD_CLK`,
`RX1`, and `TX1`, but does not show the actual ESP32-S3 GPIO numbers behind
those nets.

## Photo-to-firmware mapping

| Board silk | Firmware meaning | Current status |
| --- | --- | --- |
| `LCD_DIO` | `DISPLAY_MOSI_PIN` | GPIO unknown |
| `LCD_CLK` | `DISPLAY_CLK_PIN` | GPIO unknown |
| `LCD_SDC` | `DISPLAY_DC_PIN` | GPIO unknown |
| `LCD_CS` | `DISPLAY_CS_PIN` | GPIO unknown |
| `LCD_RST` | `DISPLAY_RST_PIN` | GPIO unknown |
| `LCD_FMARK` | LCD TE/frame-mark | unused for SPI LCD |
| `TX1/RX1` or `TX_DBG/RX_DBG` | possible ML307/debug UART nets | GPIO unknown |
| `GPIO1/2/3` | draft external I2S mic route | GPIO visible, not verified |
| `GPIO4/5/6` | draft external MAX98357A route | GPIO visible, not verified |

## Bring-up order

1. Build and boot the draft board over Wi-Fi with LCD disabled.
2. Confirm serial logs and wake-word/audio routing.
3. Fill the real LCD GPIO numbers in `config.h`, set `LELIAN_LCD_PIN_MAP_READY`
   to `1`, and rebuild.
4. Confirm the modem UART GPIO numbers, then add a real 4G version derived from
   `DualNetworkBoard`.

The draft firmware exposes an MCP tool:

`self.board.lelian_pin_map.get_status`

Use it after boot to inspect which routes are still placeholders.
