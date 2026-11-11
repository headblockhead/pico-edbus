#ifndef EDBUS_H
#define EDBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// edbus_message_t is a type of array containing a complete edbus message.
// edbus_message_t includes the data and CRC.
typedef uint32_t edbus_message_t[8];

// edbus_message_handler_t is a type of function which is called from within an
// IRQ whenever a valid incoming message is received on the edbus.
typedef void (*edbus_message_handler_t)(const uint32_t data[static 7]);

// edbus_event_t is a type of enum which describes notable events occurring
// related to the edbus.
typedef enum edbus_event {
  EDBUS_EVENT_RX_RECV, // Occurs when an incoming message is received,
                       // regardless of its validity.
                       // EDBUS_EVENT_RX_RECV is signaled before any
                       // edbus_message_handler_t would be called.

  EDBUS_EVENT_RX_BAD_CRC, // Occurs when an incoming message fails CRC checking,
                          // so is considered invalid and is discarded.
                          // EDBUS_EVENT_RX_BAD_CRC is signaled after
                          // EDBUS_EVENT_RX_RECV, instead of calling any
                          // edbus_message_handler_t.

  EDBUS_EVENT_TX_ARBITRATION_LOSS_RETRY, // Occurs when an outgoing message's
                                         // transmission will be reattempted due
                                         // to a higher priority message taking
                                         // precedence during the current
                                         // transmission attempt.

  EDBUS_EVENT_TX_DONE, // Occurs when an outgoing message has been successfully
                       // transmitted.
} edbus_event_t;

// edbus_event_handler_t is a type of function which is called from within an
// IRQ to alert to a notable event occurring related to the edbus.
typedef void (*edbus_event_handler_t)(edbus_event_t event);

// edbus_config_t is a type of struct containing edbus's internal configuration.
typedef struct edbus_config {
  unsigned pio_num;
  unsigned rx_sm;
  unsigned tx_sm;
  unsigned pio_irq_num;

  unsigned dma_chan_rx;
  unsigned dma_chan_tx;
  unsigned dma_irq_num;

  unsigned pin_ebd;
  unsigned pin_ebc;

  edbus_message_handler_t message_handler;
  edbus_event_handler_t event_handler;
} edbus_config_t;

// edbus_get_default_config returns an edbus_config struct with default values.
// pin_ebc must be sequentially after (+1 from) pin_ebd.
// By default, edbus uses PIO 0, running on state machines 0 and 1, using PIO
// NVIC IRQ 0, and uses DMA channels 0 and 1, DMA NVIC IRQ 0, and the DMA
// sniffer.
edbus_config_t edbus_get_default_config(unsigned int pin_ebd,
                                        unsigned int pin_ebc,
                                        edbus_message_handler_t message_handler,
                                        edbus_event_handler_t event_handler);

// edbus_config_set_pio sets the PIO and state machines to be used for edbus.
void edbus_config_set_pio(edbus_config_t *c, unsigned int pio_num,
                          unsigned int rx_sm, unsigned int tx_sm);

// edbus_config_set_pio_irq sets the PIO IRQ number to be used for edbus.
void edbus_config_set_pio_irq(edbus_config_t *c, unsigned int pio_irq_num);

// edbus_config_set_dma sets the DMA channels to be used for edbus.
// The DMA sniffer is used by edbus.
void edbus_config_set_dma(edbus_config_t *c, unsigned int dma_chan_rx,
                          unsigned int dma_chan_tx, unsigned int dma_irq_num);

// edbus_config_set_pins sets the pins to be used for edbus.
void edbus_config_set_pins(edbus_config_t *c, unsigned int pin_ebd,
                           unsigned int pin_ebc);

// edbus_config_set_message_handler sets the message handler to be used for
// edbus.
void edbus_config_set_message_handler(edbus_config_t *c,
                                      edbus_message_handler_t message_handler);

// edbus_config_set_event_handler sets the event handler to be used for edbus.
void edbus_config_set_event_handler(edbus_config_t *c,
                                    edbus_event_handler_t event_handler);

// edbus_init initializes the edbus with the given configuration.
void edbus_init(const edbus_config_t *c);

// edbus_insert_crc calculates the CRC for the provided edbus message, and
// inserts it into the last word of the message.
void edbus_insert_crc(edbus_message_t message);

// edbus_insert_crc_send_blocking calculates the CRC for the provided edbus
// message, inserts it, and sends it.
// edbus_insert_crc_send_blocking will block until any message currently being
// transmitted is done, however edbus_insert_crc_send_blocking will not block
// for the new message to be fully transmitted.
void edbus_insert_crc_send_blocking(const edbus_message_t message);

// edbus_send_blocking sends an edbus message.
// For edbus_send_blocking to function correctly, the message must already have
// a valid CRC in the last word.
// edbus_send_blocking will block until any message currently being transmitted
// is done, however edbus_send_blocking will not block for the new message to be
// fully transmitted.
void edbus_send_blocking(const edbus_message_t message);

#endif
