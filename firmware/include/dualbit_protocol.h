/*
 * =====================================================================
 *  DualBit Bridge — Protocolo serial binário ESP32 <-> micro:bit
 * =====================================================================
 *  Este header é a ÚNICA fonte de verdade do protocolo. Ele é espelhado
 *  em TypeScript na extensão PXT (extension/main.ts). Se alterar algo
 *  aqui, altere também lá.
 *
 *  TX  (ESP32 -> micro:bit) : frame começa com 0xAA
 *  RX  (micro:bit -> ESP32) : frame começa com 0x55
 * =====================================================================
 */

#ifndef DUALBIT_PROTOCOL_H
#define DUALBIT_PROTOCOL_H

/* --------------------------------------------------------------------- */
/*  Bytes mágicos de sincronismo                                          */
/* --------------------------------------------------------------------- */
#define DB_TX_HEADER 0xAA   /* ESP32 -> micro:bit */
#define DB_HOST_HEADER 0x55 /* micro:bit -> ESP32 */

/* --------------------------------------------------------------------- */
/*  Tipos de pacote TX (ESP32 -> micro:bit)                               */
/* --------------------------------------------------------------------- */
#define DB_PKT_INPUT 0x01  /* estado dos botões/eixos/bateria (14 bytes) */
#define DB_PKT_MOTION 0x02 /* acelerômetro/giroscópio      (12 bytes) */
#define DB_PKT_MAC 0x03    /* resposta de leitura do MAC    (9 bytes) */
#define DB_PKT_ACK 0x04    /* confirmação de comando        (5 bytes) */

/* Tamanho TOTAL do frame, incluindo header, tipo e checksum */
#define DB_PKT_INPUT_LEN 14
#define DB_PKT_MOTION_LEN 12
#define DB_PKT_MAC_LEN 9
#define DB_PKT_ACK_LEN 5

/* --------------------------------------------------------------------- */
/*  Comandos RX (micro:bit -> ESP32)                                      */
/* --------------------------------------------------------------------- */
#define DB_CMD_SET_MAC 0x01     /* payload: 6 bytes                        */
#define DB_CMD_GET_MAC 0x02     /* payload: 0                              */
#define DB_CMD_RUMBLE 0x03      /* payload: small, large (0..255)          */
#define DB_CMD_LED_COLOR 0x04   /* payload: r, g, b                        */
#define DB_CMD_LED_FLASH 0x05   /* payload: r, g, b, onTicks, offTicks     */
#define DB_CMD_LED_OFF 0x06     /* payload: 0                              */
#define DB_CMD_RUMBLE_OFF 0x07  /* payload: 0                              */
#define DB_CMD_REBOOT 0x08      /* payload: 0                              */
#define DB_CMD_SET_DEADZONE 0x09/* payload: 0..64                          */
#define DB_CMD_SET_MOTION 0x0A  /* payload: 0 ou 1                         */
#define DB_CMD_SET_HEARTBEAT 0x0B /* payload: ticks de 10 ms (1..255)      */
#define DB_CMD_PING 0x0C        /* payload: 0                              */
#define DB_CMD_CLEAR_MAC 0x0D   /* payload: 0 (apaga MAC e reinicia)       */

/* Capacidade máxima do parser de comandos no ESP32 */
#define DB_HOST_MAX_PAYLOAD 16

/* Resultados do ACK */
#define DB_ACK_OK 0
#define DB_ACK_BAD_CMD 1
#define DB_ACK_BAD_PARAM 2
#define DB_ACK_REBOOTING 3

/* --------------------------------------------------------------------- */
/*  Máscara de botões digitais (16 bits, little-endian: byte baixo 1º)    */
/* --------------------------------------------------------------------- */
#define DB_BTN_CROSS 0
#define DB_BTN_CIRCLE 1
#define DB_BTN_SQUARE 2
#define DB_BTN_TRIANGLE 3
#define DB_BTN_L1 4
#define DB_BTN_R1 5
#define DB_BTN_L3 6
#define DB_BTN_R3 7
#define DB_BTN_SHARE 8
#define DB_BTN_OPTIONS 9
#define DB_BTN_PS 10
#define DB_BTN_TOUCHPAD 11
#define DB_BTN_UP 12
#define DB_BTN_DOWN 13
#define DB_BTN_LEFT 14
#define DB_BTN_RIGHT 15

#define DB_BTN_COUNT 16

/* --------------------------------------------------------------------- */
/*  Flags de status (byte 12 do pacote INPUT)                             */
/* --------------------------------------------------------------------- */
#define DB_FLAG_CONNECTED 0x01
#define DB_FLAG_CHARGING 0x02
#define DB_FLAG_AUDIO 0x04
#define DB_FLAG_MIC 0x08
#define DB_FLAG_MOTION 0x10

/* Bateria desconhecida */
#define DB_BATTERY_UNKNOWN 0xFF

#endif /* DUALBIT_PROTOCOL_H */
