# STM32 Nucleo-U385RG-Q UART Bridge

A standalone Zephyr application that turns a **Nucleo-U385RG-Q** into a
transparent UART bridge between your laptop and an **AST2700**.

```
 laptop  <== USB / ST-Link VCP ==>  usart1 (host)  [ STM32U385 ]  usart3 (target)  <== UART ==>  AST2700
```

Every byte in on one side goes out the other side, both directions, with no
echo, framing, or interpretation. The Zephyr console, shell, boot banner and
logging are all disabled so nothing pollutes the byte stream.

## Wiring

| STM32 pin        | Signal        | Connect to        |
| ---------------- | ------------- | ----------------- |
| PA9  (USART1_TX) | host TX       | ST-Link (on-board, via USB) |
| PA10 (USART1_RX) | host RX       | ST-Link (on-board, via USB) |
| PC10 (USART3_TX) | target TX     | AST2700 UART **RX** |
| PC11 (USART3_RX) | target RX     | AST2700 UART **TX** |
| GND              | ground        | AST2700 **GND**   |

Notes:
- USART1 is already routed to the on-board ST-Link Virtual COM Port, so the
  laptop side needs only the USB cable.
- USART3 is on the ST morpho header. PC10/PC11 are 3.3V TTL — do **not** wire
  them to an RS-232 or 5V level; use a level shifter if the AST2700 side differs.
- TX-to-RX is crossed (STM32 TX -> AST2700 RX and vice-versa). Always share GND.

## Build & flash

This is a standalone Zephyr app. From a Zephyr workspace (with `west` and the
`nucleo_u385rg_q` board available):

```bash
west build -b nucleo_u385rg_q app/uart_bridge
west flash
```

## Use it

Open the ST-Link VCP on your laptop at **115200 8N1** and you are talking
straight to the AST2700:

```bash
# Linux (device name may vary)
picocom -b 115200 /dev/ttyACM0
# or
minicom -D /dev/ttyACM0 -b 115200
```

The user LED (LD4) blinks at ~1 Hz to show the bridge is running.

## Changing baud rate

Both sides default to 115200 8N1. To change the AST2700 side, edit
`current-speed` in `app.overlay` (the `&usart3` node). To change the laptop
side, override `&usart1` the same way (and match your terminal).
