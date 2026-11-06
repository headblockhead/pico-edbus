#include "edbus.h"
#include "crc32.h"
#include "edbus.pio.h"

#include <hardware/dma.h>
#include <hardware/pio.h>
#include <pico/sem.h>
#include <pico/stdlib.h>

#include <stdio.h>
#include <string.h>

// edbus_config structs should only be manipulated using the set functions, so
// the full struct definition is kept out of the public header.
struct edbus_config {
  PIO pio;
  uint rx_sm;
  uint tx_sm;
  uint pio_irq_num;

  uint dma_chan_rx;
  uint dma_chan_tx;
  uint dma_irq_num;

  uint pin_ebd;
  uint pin_ebc;

  edbus_message_handler_t message_handler;
  edbus_event_handler_t event_handler;
};

// message_outbound is a buffer where messages are stored before transmission.
// This is useful for automatic retransmission from arbitration losses.
static edbus_message_t message_outbound;
// message_outbound_semaphore controls access to the message_outbound buffer, so
// that multiple threads do not attempt to write to it at the same time, and so
// that it is not altered until transmission is complete.
static semaphore_t message_outbound_semaphore;

// message_inbound is a buffer where message are stored after reception.
static edbus_message_t message_inbound;

// config is an instance of an edbus_config struct, which holds the values used
// to run the edbus.
static edbus_config_t config;

// check_config is a debugging tool that uses assertions to check that config
// values are correct. As check_config uses pico-sdk functions to do this, the
// pico-sdk's compile flags for PARAM_ASSERTIONS_ENABLED apply here.
// This means check_config only has an effect when assertions are explicitly
// enabled during a debug build.
static void check_config(const edbus_config_t *c) {
  check_pio_param(c->pio);
  check_sm_param(c->rx_sm);
  check_sm_param(c->tx_sm);
  invalid_params_if(HARDWARE_PIO, c->pio_irq_num >= NUM_PIO_IRQS);

  check_dma_channel_param(c->dma_chan_rx);
  check_dma_channel_param(c->dma_chan_tx);
  invalid_params_if(HARDWARE_DMA, c->dma_irq_num >= NUM_DMA_IRQS);

  check_pio_pin_param(c->pin_ebd);
  check_pio_pin_param(c->pin_ebc);
  invalid_params_if(HARDWARE_PIO, c->pin_ebd + 1 != c->pin_ebc);
}

edbus_config_t edbus_get_default_config(unsigned int pin_ebd,
                                        unsigned int pin_ebc,
                                        edbus_message_handler_t message_handler,
                                        edbus_event_handler_t event_handler) {
  return (edbus_config_t){
      // Defaults:

      // PIO 0, state machines 0 and 1.
      .pio = pio0,
      .rx_sm = 0,
      .tx_sm = 1,

      // PIO system IRQ 0.
      // (note: this number is different to the IRQ number used inside of PIO
      // programs)
      .pio_irq_num = 0,

      // DMA channels 0 and 1.
      .dma_chan_rx = 0,
      .dma_chan_tx = 1,

      // DMA system IRQ 0.
      .dma_irq_num = 0,

      // Pins and handlers have no default value, and must be provided.
      .pin_ebd = pin_ebd,
      .pin_ebc = pin_ebc,
      .message_handler = message_handler,
      .event_handler = event_handler,
  };
}

void edbus_config_set_pio(edbus_config_t *c, unsigned int pio_num,
                          unsigned int rx_sm, unsigned int tx_sm) {
  c->pio = PIO_INSTANCE(pio_num);
  c->rx_sm = rx_sm;
  c->tx_sm = tx_sm;
  check_config(c);
}

void edbus_config_set_pio_irq(edbus_config_t *c, unsigned int pio_irq_num) {
  c->pio_irq_num = pio_irq_num;
  check_config(c);
}

void edbus_config_set_dma(edbus_config_t *c, unsigned int dma_chan_rx,
                          unsigned int dma_chan_tx, unsigned int dma_irq_num) {
  c->dma_chan_rx = dma_chan_rx;
  c->dma_chan_tx = dma_chan_tx;
  c->dma_irq_num = dma_irq_num;
  check_config(c);
}

void edbus_config_set_pins(edbus_config_t *c, unsigned int pin_ebd,
                           unsigned int pin_ebc) {
  c->pin_ebd = pin_ebd;
  c->pin_ebc = pin_ebc;
  check_config(c);
}

void edbus_config_set_message_handler(edbus_config_t *c,
                                      edbus_message_handler_t message_handler) {
  c->message_handler = message_handler;
}

void edbus_config_set_event_handler(edbus_config_t *c,
                                    edbus_event_handler_t event_handler) {
  c->event_handler = event_handler;
}

// init_pio_program_rx writes the RX program into the PIO instruction memory,
// and calls the program's init function.
static void init_pio_program_rx() {
  uint offset_rx = pio_add_program(config.pio, &edbus_rx_program);
  edbus_rx_program_init(config.pio, config.rx_sm, offset_rx, config.pin_ebd,
                        config.pin_ebc);
}

// init_pio_program_tx writes the TX program into the PIO instruction memory,
// and calls the program's init function.
static void init_pio_program_tx() {
  uint offset_tx = pio_add_program(config.pio, &edbus_tx_program);
  edbus_tx_program_init(config.pio, config.tx_sm, offset_tx, config.pin_ebd,
                        config.pin_ebc);
}

// pio_get_interrupt_source converts an IRQ number in a PIO block into an
// interrupt source number.
static pio_interrupt_source_t pio_get_interrupt_source(uint interrupt) {
#if PICO_PIO_VERSION > 0
  invalid_params_if(HARDWARE_PIO, interrupt > 7);
#else
  invalid_params_if(HARDWARE_PIO, interrupt > 3);
#endif
  return (pio_interrupt_source_t)(pis_interrupt0 + interrupt);
}

// pio_irq_handler is called when the edbus PIO block raises an IRQ.
static void pio_irq_handler() {
  if (pio_interrupt_get(config.pio, edbus_tx_error_irq)) {
    // RP2040-E13 and RP2350-E5
    hw_clear_bits(&dma_hw->ch[config.dma_chan_tx].al1_ctrl,
                  DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_irqn_set_channel_enabled(config.dma_irq_num, config.dma_chan_tx, false);
    dma_channel_abort(config.dma_chan_tx);
    dma_irqn_acknowledge_channel(config.dma_irq_num, config.dma_chan_tx);
    dma_irqn_set_channel_enabled(config.dma_irq_num, config.dma_chan_tx, true);
    hw_set_bits(&dma_hw->ch[config.dma_chan_tx].al1_ctrl,
                DMA_CH0_CTRL_TRIG_EN_BITS);

    pio_sm_clear_fifos(config.pio, config.tx_sm);

    pio_sm_exec(pio, sm,
                pio_encode_set(pio_x, 7) | pio_encode_sideset(2, 0b11));

    config.event_handler(EDBUS_EVENT_TX_ARBITRATION_LOSS_RETRY);

    dma_channel_set_read_addr(config.dma_chan_tx, message_outbound, true);

    pio_interrupt_clear(config.pio, edbus_tx_error_irq);
  }
  if (pio_interrupt_get(config.pio, edbus_tx_done_irq)) {
    pio_interrupt_clear(config.pio, edbus_tx_done_irq);
    sem_release(&message_outbound_semaphore);
    config.event_handler(EDBUS_EVENT_TX_DONE);
    // The message_outbound_semaphore can be released after the message is
    // finished transmitting, as it is no longer possible for an arbitration
    // loss error to require the message stored in the message_outbound buffer
    // to be retransmitted.
  }
}

// init_pio_irq configures the PIO to trigger an interrupt when the error_irq or
// done_irq are raised inside of the edbus programs, and adds pio_irq_handler as
// a handler for these interrupts.
static void init_pio_irq() {
  pio_interrupt_source_t irq_source_tx_error =
      pio_get_interrupt_source(edbus_tx_error_irq);
  pio_interrupt_source_t irq_source_tx_done =
      pio_get_interrupt_source(edbus_tx_done_irq);
  uint32_t irq_source_mask =
      (1 << irq_source_tx_error) | (1 << irq_source_tx_done);
  pio_set_irqn_source_mask_enabled(config.pio, config.pio_irq_num,
                                   irq_source_mask, true);

  uint nvic_irq_num_pio = pio_get_irq_num(config.pio, config.pio_irq_num);
  irq_add_shared_handler(nvic_irq_num_pio, pio_irq_handler,
                         PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_enabled(nvic_irq_num_pio, true);
}

// init_dma_rx sets up the DMA channel used to transfer incoming messages out of
// the RX PIO state machine and into the message_inbound buffer, and configures
// the DMA sniffer to report CRC32s of data transferred through the RX DMA
// channel.
static void init_dma_rx() {
  dma_channel_claim(config.dma_chan_rx);
  dma_channel_config dma_config_rx =
      dma_channel_get_default_config(config.dma_chan_rx);
  channel_config_set_transfer_data_size(&dma_config_rx, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_config_rx, false);
  channel_config_set_write_increment(&dma_config_rx, true);
  uint dreq_rx = pio_get_dreq(config.pio, config.rx_sm, false);
  channel_config_set_dreq(&dma_config_rx, dreq_rx);
  channel_config_set_sniff_enable(&dma_config_rx, true);
  dma_sniffer_set_data_accumulator(0xFFFFFFFF);
  dma_sniffer_set_byte_swap_enabled(true);
  dma_sniffer_enable(config.dma_chan_rx, DMA_SNIFF_CTRL_CALC_VALUE_CRC32, true);
  dma_channel_configure(config.dma_chan_rx, &dma_config_rx, message_inbound,
                        &config.pio->rxf[config.rx_sm],
                        dma_encode_transfer_count(8), false);
}

// init_dma_tx sets up the DMA channel used to transfer outgoing messages out of
// the message_outbound buffer and into the TX PIO state machine.
static void init_dma_tx() {
  dma_channel_claim(config.dma_chan_tx);
  dma_channel_config dma_config_tx =
      dma_channel_get_default_config(config.dma_chan_tx);
  channel_config_set_transfer_data_size(&dma_config_tx, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_config_tx, true);
  channel_config_set_write_increment(&dma_config_tx, false);
  uint dreq_tx = pio_get_dreq(config.pio, config.tx_sm, true);
  channel_config_set_dreq(&dma_config_tx, dreq_tx);
  dma_channel_configure(config.dma_chan_tx, &dma_config_tx,
                        &config.pio->txf[config.tx_sm], message_outbound,
                        dma_encode_transfer_count(8), false);
}

// dma_irq_handler is called when a DMA channel raises an IRQ.
static void dma_irq_handler() {
  if (dma_irqn_get_channel_status(config.dma_irq_num, config.dma_chan_rx)) {
    dma_irqn_acknowledge_channel(config.dma_irq_num, config.dma_chan_rx);
    config.event_handler(EDBUS_EVENT_RX_RECV);

    uint32_t crc = dma_sniffer_get_data_accumulator();
    if (crc == 0) {
      config.message_handler(message_inbound);
    } else {
      config.event_handler(EDBUS_EVENT_RX_BAD_CRC);
    }
    dma_sniffer_set_data_accumulator(0xFFFFFFFF);
    dma_channel_set_write_addr(config.dma_chan_rx, message_inbound, true);
  };
  if (dma_irqn_get_channel_status(config.dma_irq_num, config.dma_chan_tx)) {
    dma_irqn_acknowledge_channel(config.dma_irq_num, config.dma_chan_tx);
  };
}

// init_dma_irq configures the DMA channels to trigger interrupts when
// completing their transfers, and adds dma_irq_handler as a handler for these
// interrupts.
static void init_dma_irq() {
  dma_irqn_set_channel_enabled(config.dma_irq_num, config.dma_chan_rx, true);
  dma_irqn_set_channel_enabled(config.dma_irq_num, config.dma_chan_tx, true);

  uint nvic_irq_num_dma = dma_get_irq_num(config.dma_irq_num);
  irq_add_shared_handler(nvic_irq_num_dma, dma_irq_handler,
                         PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_enabled(nvic_irq_num_dma, true);
}

void edbus_init(const edbus_config_t *c) {
  check_config(c);
  config = *c;

  sem_init(&message_outbound_semaphore, 1, 1);

  init_pio_program_rx();
  init_pio_program_tx();
  init_pio_irq();
  init_dma_rx();
  init_dma_tx();
  init_dma_irq();

  dma_channel_start(config.dma_chan_rx);
}

void edbus_insert_crc(edbus_message_t message) {
  uint32_t crc = 0xFFFFFFFF;
  for (int i = 0; i < 7; i++) {
    crc ^= message[i];
    crc = (crc << 8) ^ crc32_table[crc >> 24];
    crc = (crc << 8) ^ crc32_table[crc >> 24];
    crc = (crc << 8) ^ crc32_table[crc >> 24];
    crc = (crc << 8) ^ crc32_table[crc >> 24];
  }
  message[7] = crc;
}

void edbus_insert_crc_send_blocking(const edbus_message_t message) {
  sem_acquire_blocking(&message_outbound_semaphore);
  memcpy(message_outbound, message, sizeof(edbus_message_t));
  edbus_insert_crc(message_outbound);
  dma_channel_set_read_addr(config.dma_chan_tx, message_outbound, true);
}

void edbus_send_blocking(const edbus_message_t message) {
  sem_acquire_blocking(&message_outbound_semaphore);
  memcpy(message_outbound, message, sizeof(edbus_message_t));
  dma_channel_set_read_addr(config.dma_chan_tx, message_outbound, true);
}
