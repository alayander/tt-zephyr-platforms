/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bidirectional UART bridge for the STM32 Nucleo-U385RG-Q.
 *
 * The STM32 sits between your laptop and an AST2700:
 *
 *   laptop  <--USB/VCP-->  usart1 (host)  [STM32]  usart3 (target)  <--UART-->  AST2700
 *
 * Every byte received on one side is forwarded out the other side, both
 * directions, interrupt-driven with ring buffers so nothing is dropped at
 * 115200 (assuming the far end honours the same rate). No framing, no echo,
 * no interpretation: it is a transparent wire.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/ring_buffer.h>

#define HOST_UART_NODE   DT_ALIAS(host_uart)
#define TARGET_UART_NODE DT_ALIAS(target_uart)

BUILD_ASSERT(DT_NODE_HAS_STATUS(HOST_UART_NODE, okay), "host-uart alias is not enabled");
BUILD_ASSERT(DT_NODE_HAS_STATUS(TARGET_UART_NODE, okay), "target-uart alias is not enabled");

#define RING_BUF_SIZE 2048
#define CHUNK_SIZE    64

/* host->target and target->host byte streams. */
RING_BUF_DECLARE(h2t_rb, RING_BUF_SIZE);
RING_BUF_DECLARE(t2h_rb, RING_BUF_SIZE);

static const struct device *const host_dev = DEVICE_DT_GET(HOST_UART_NODE);
static const struct device *const target_dev = DEVICE_DT_GET(TARGET_UART_NODE);

#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

/*
 * Shared ISR body for one UART endpoint.
 *   rx_rb : where bytes received on this UART are queued
 *   tx_rb : where this UART pulls bytes to transmit
 *   peer  : the other UART, whose TX we kick when we receive data
 */
static void bridge_service(const struct device *dev, struct ring_buf *rx_rb,
			   struct ring_buf *tx_rb, const struct device *peer)
{
	while (true) {
		uart_irq_update(dev);
		if (!uart_irq_is_pending(dev)) {
			break;
		}

		if (uart_irq_rx_ready(dev)) {
			uint8_t *dst;
			uint32_t space = ring_buf_put_claim(rx_rb, &dst, CHUNK_SIZE);

			if (space == 0) {
				/* rx buffer full: drain FIFO into the void so the
				 * interrupt clears rather than spinning forever.
				 */
				uint8_t drop[CHUNK_SIZE];

				(void)uart_fifo_read(dev, drop, sizeof(drop));
			} else {
				int n = uart_fifo_read(dev, dst, space);

				ring_buf_put_finish(rx_rb, n < 0 ? 0 : n);
				if (n > 0) {
					uart_irq_tx_enable(peer);
				}
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *src;
			uint32_t avail = ring_buf_get_claim(tx_rb, &src, CHUNK_SIZE);

			if (avail == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int n = uart_fifo_fill(dev, src, avail);

				ring_buf_get_finish(tx_rb, n < 0 ? 0 : n);
			}
		}
	}
}

static void host_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	bridge_service(dev, &h2t_rb, &t2h_rb, target_dev);
}

static void target_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	bridge_service(dev, &t2h_rb, &h2t_rb, host_dev);
}

int main(void)
{
	if (!device_is_ready(host_dev) || !device_is_ready(target_dev)) {
		return -ENODEV;
	}

	uart_irq_rx_disable(host_dev);
	uart_irq_tx_disable(host_dev);
	uart_irq_rx_disable(target_dev);
	uart_irq_tx_disable(target_dev);

	uart_irq_callback_user_data_set(host_dev, host_isr, NULL);
	uart_irq_callback_user_data_set(target_dev, target_isr, NULL);

	uart_irq_rx_enable(host_dev);
	uart_irq_rx_enable(target_dev);

#if DT_NODE_EXISTS(DT_ALIAS(led0))
	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif

	while (1) {
#if DT_NODE_EXISTS(DT_ALIAS(led0))
		(void)gpio_pin_toggle_dt(&led);
#endif
		k_sleep(K_MSEC(500));
	}

	return 0;
}
