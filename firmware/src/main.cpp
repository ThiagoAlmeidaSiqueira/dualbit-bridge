/*
 * =====================================================================
 *  DualBit Bridge — Firmware do ESP32
 * =====================================================================
 *  Ponte entre um controle PlayStation 4 (DualShock 4) via Bluetooth
 *  Classic e um BBC micro:bit via UART, usando um protocolo binário
 *  compacto (sem JSON/strings).
 *
 *  Fluxo:
 *     DualShock 4  --BT-->  ESP32  --UART 115200-->  micro:bit (MakeCode)
 *     DualShock 4  <--BT--  ESP32  <--UART 115200--  micro:bit (comandos)
 *
 *  Recursos:
 *    - MAC do controle persistido em NVS (Preferences)
 *    - Leitura completa de botões, eixos, gatilhos, bateria e status
 *    - Rumble (motor fraco/forte) e lightbar RGB com flash
 *    - Envio orientado a eventos com deadzone + heartbeat (throttling)
 *
 *  Dependência: https://github.com/aed3/PS4-esp32  (PS4Controller)
 *  Requer ESP32 com Bluetooth Classic (ESP32 "clássico"; NÃO use S2/S3/C3).
 * =====================================================================
 */

#include <Arduino.h>
#include <Preferences.h>
#include <PS4Controller.h>
#include <driver/gpio.h>
#include <esp_mac.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dualbit_protocol.h"

/* ===================================================================== */
/*                          CONFIGURAÇÃO                                 */
/* ===================================================================== */

#ifndef DUALBIT_UART_NUM
#define DUALBIT_UART_NUM 2 /* UART2 = livre para a ponte (Serial = debug) */
#endif

#ifndef DUALBIT_PIN_RX
#define DUALBIT_PIN_RX 16 /* ESP32 RX2  <- micro:bit TX (pin 1 / P1) */
#endif

#ifndef DUALBIT_PIN_TX
#define DUALBIT_PIN_TX 17 /* ESP32 TX2  -> micro:bit RX (pin 2 / P2) */
#endif

#ifndef DUALBIT_BAUD
#define DUALBIT_BAUD 115200
#endif

/* Giroscópio/acelerômetro: desligado por padrão.
 *
 * IMPORTANTE: no fork aed3/PS4-esp32 o parse dos sensores está COMENTADO
 * em src/ps4_parser.c (`ps4.sensor = parsePacketSensor(packet);`). Para
 * habilitar de verdade você precisa:
 *   1) descomentar essa linha e o corpo de parsePacketSensor();
 *   2) compilar o firmware com -DDUALBIT_ENABLE_MOTION=1.
 * Caso contrário os valores lidos são lixo de memória. Ver docs/PROTOCOL.md.
 */
#ifndef DUALBIT_ENABLE_MOTION
#define DUALBIT_ENABLE_MOTION 0
#endif

#define DB_DEBUG_BAUD 115200

#define DB_DEFAULT_DEADZONE 4    /* unidades do eixo (-128..127) */
#define DB_DEFAULT_HB_TICKS 5    /* x10 ms = 50 ms (20 Hz) */
#define DB_MOTION_PERIOD_MS 100  /* 10 Hz quando o motion está ligado */
#define DB_OUTPUT_REFRESH_MS 2000 /* reenvia rumble/LED periodicamente */
#define DB_OUTPUT_MIN_GAP_MS 20   /* evita congestão no L2CAP do BT */
#define DB_AXIS_HYSTERESIS 2      /* ignora jitter de +/-2 unidades */

/* Namespace/keys da NVS (Preferences limita a 15 caracteres) */
#define DB_NVS_NAMESPACE "dualbit"
#define DB_NVS_KEY_MAC "ps4mac"
#define DB_NVS_KEY_DZ "deadzone"
#define DB_NVS_KEY_HB "heartbeat"
#define DB_NVS_KEY_MOTION "motion"

#define DB_LOG(...) Serial.printf(__VA_ARGS__)

/* ===================================================================== */
/*                       ESTADO GLOBAL                                   */
/* ===================================================================== */

static HardwareSerial BridgeSerial(DUALBIT_UART_NUM);
static Preferences prefs;

/* ---------------------------------------------------------------------
 * MAC de host (override opcional), persistido na NVS.
 *
 * IMPORTANTE — semântica da biblioteca aed3/PS4-esp32:
 * o argumento de PS4.begin(mac) NÃO é o MAC do controle. A lib chama
 * esp_base_mac_addr_set() para mudar o MAC Bluetooth do PRÓPRIO ESP32.
 * Isso existe porque o DualShock 4 só conecta ao host cujo endereço ele
 * memorizou (normalmente o do console PS4, gravado quando o controle foi
 * ligado por USB no console).
 *
 * Logo:
 *   vazio  -> PS4.begin()  = pareamento aberto (PS + Share uma vez)
 *   setado -> PS4.begin(x) = ESP32 se apresenta como "x"
 * ------------------------------------------------------------------- */
static String savedMac;                       /* "aa:bb:cc:dd:ee:ff" ou "" */
static uint8_t deadzone = DB_DEFAULT_DEADZONE;
static uint8_t heartbeatTicks = DB_DEFAULT_HB_TICKS;
static bool motionEnabled = false;

/* Saídas (feedback) — sempre enviadas via PS4.sendToController() */
static uint8_t rumbleSmall = 0; /* motor fraco (direito) */
static uint8_t rumbleLarge = 0; /* motor forte (esquerdo) */
static uint8_t ledR = 0, ledG = 64, ledB = 255; /* lightbar padrão: azul */
static uint8_t flashOnTicks = 0;  /* 255 ticks = 2550 ms */
static uint8_t flashOffTicks = 0;
static bool outputDirty = true;

static uint32_t lastOutputMs = 0;
static uint32_t rebootAt = 0;
static uint32_t lastInputSentMs = 0;
#if DUALBIT_ENABLE_MOTION
static uint32_t lastMotionSentMs = 0;
#endif
static uint8_t txSeq = 0;

/* Snapshot do estado lido do controle */
struct DbInput {
  uint16_t buttons;
  int8_t lx, ly, rx, ry;
  uint8_t l2, r2;
  uint8_t battery;
  uint8_t flags;
};

static DbInput curInput;
static DbInput prevInput;

/* Parser de comandos vindos do micro:bit */
static uint8_t hostBuf[3 + DB_HOST_MAX_PAYLOAD + 1];
static uint8_t hostLen = 0;

/* Console de debug (USB) */
static char dbgLine[64];
static uint8_t dbgLen = 0;

/* ===================================================================== */
/*                     UTILITÁRIOS DE MAC / NVS                          */
/* ===================================================================== */

static bool parseMac(const char *text, uint8_t out[6]) {
  unsigned v[6];

  if (text == nullptr || strlen(text) != 17) return false;
  if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4],
             &v[5]) != 6) {
    return false;
  }
  for (uint8_t i = 0; i < 6; i++) {
    if (v[i] > 0xFF) return false;
    out[i] = (uint8_t)v[i];
  }
  return true;
}

static String formatMac(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

static void loadConfig() {
  prefs.begin(DB_NVS_NAMESPACE, true); /* read-only */
  savedMac = prefs.getString(DB_NVS_KEY_MAC, "");
  deadzone = prefs.getUChar(DB_NVS_KEY_DZ, DB_DEFAULT_DEADZONE);
  heartbeatTicks = prefs.getUChar(DB_NVS_KEY_HB, DB_DEFAULT_HB_TICKS);
  motionEnabled = prefs.getBool(DB_NVS_KEY_MOTION, false);
  prefs.end();

  if (deadzone > 64) deadzone = 64;
  if (heartbeatTicks == 0) heartbeatTicks = DB_DEFAULT_HB_TICKS;
}

static void saveMacToNvs(const String &mac) {
  prefs.begin(DB_NVS_NAMESPACE, false);
  if (mac.length() == 17) {
    prefs.putString(DB_NVS_KEY_MAC, mac);
  } else {
    prefs.remove(DB_NVS_KEY_MAC);
  }
  prefs.end();
}

static void saveTuningToNvs() {
  prefs.begin(DB_NVS_NAMESPACE, false);
  prefs.putUChar(DB_NVS_KEY_DZ, deadzone);
  prefs.putUChar(DB_NVS_KEY_HB, heartbeatTicks);
  prefs.putBool(DB_NVS_KEY_MOTION, motionEnabled);
  prefs.end();
}

/* ===================================================================== */
/*                       ENVIO DE FRAMES (TX)                            */
/* ===================================================================== */

/**
 * Monta e envia: [0xAA][tipo][payload...][XOR]
 * O XOR cobre o tipo + todo o payload (o header fica de fora).
 */
static void writeFrame(uint8_t type, const uint8_t *payload, uint8_t len) {
  if (len > DB_HOST_MAX_PAYLOAD) return;

  uint8_t frame[2 + DB_HOST_MAX_PAYLOAD + 1];
  uint8_t n = 0;

  frame[n++] = DB_TX_HEADER;
  frame[n++] = type;

  uint8_t cs = type;
  for (uint8_t i = 0; i < len; i++) {
    frame[n++] = payload[i];
    cs ^= payload[i];
  }
  frame[n++] = cs;

  BridgeSerial.write(frame, n);
}

static void sendAck(uint8_t cmd, uint8_t result) {
  const uint8_t payload[2] = {cmd, result};
  writeFrame(DB_PKT_ACK, payload, sizeof(payload));
}

/* ===================================================================== */
/*                    LEITURA DO CONTROLE (ESP32)                        */
/* ===================================================================== */

static inline int8_t applyDeadzone(int8_t value) {
  int magnitude = value < 0 ? -(int)value : (int)value;
  if (magnitude <= (int)deadzone) return 0;
  return value;
}

static uint16_t readButtons() {
  uint16_t b = 0;

  if (PS4.Cross()) b |= (1u << DB_BTN_CROSS);
  if (PS4.Circle()) b |= (1u << DB_BTN_CIRCLE);
  if (PS4.Square()) b |= (1u << DB_BTN_SQUARE);
  if (PS4.Triangle()) b |= (1u << DB_BTN_TRIANGLE);

  if (PS4.L1()) b |= (1u << DB_BTN_L1);
  if (PS4.R1()) b |= (1u << DB_BTN_R1);
  if (PS4.L3()) b |= (1u << DB_BTN_L3);
  if (PS4.R3()) b |= (1u << DB_BTN_R3);

  if (PS4.Share()) b |= (1u << DB_BTN_SHARE);
  if (PS4.Options()) b |= (1u << DB_BTN_OPTIONS);
  if (PS4.PSButton()) b |= (1u << DB_BTN_PS);
  if (PS4.Touchpad()) b |= (1u << DB_BTN_TOUCHPAD);

  /* A biblioteca reporta D-pad cardinal OU diagonal (nunca os dois).
   * Aqui as diagonais são convertidas em duas direções simultâneas para
   * que o jogo/extensão não perca o input. */
  if (PS4.Up() || PS4.UpLeft() || PS4.UpRight()) b |= (1u << DB_BTN_UP);
  if (PS4.Down() || PS4.DownLeft() || PS4.DownRight())
    b |= (1u << DB_BTN_DOWN);
  if (PS4.Left() || PS4.UpLeft() || PS4.DownLeft()) b |= (1u << DB_BTN_LEFT);
  if (PS4.Right() || PS4.UpRight() || PS4.DownRight())
    b |= (1u << DB_BTN_RIGHT);

  return b;
}

/**
 * PS4.Battery() no fork aed3 devolve packet[42] & 0x0F, ou seja 0..10
 * (décimos). Convertemos para 0..100 %. Se você trocar de biblioteca e ela
 * já devolver 0..100, ajuste aqui (ou saturação abaixo já resolve).
 */
static uint8_t readBatteryPercent() {
  if (!PS4.isConnected()) return DB_BATTERY_UNKNOWN;

  uint16_t pct = (uint16_t)PS4.Battery() * 10u;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

static void readInput(DbInput &s) {
  const bool connected = PS4.isConnected();

  if (connected) {
    s.buttons = readButtons();
    s.lx = applyDeadzone(PS4.LStickX());
    s.ly = applyDeadzone(PS4.LStickY());
    s.rx = applyDeadzone(PS4.RStickX());
    s.ry = applyDeadzone(PS4.RStickY());
    s.l2 = PS4.L2Value();
    s.r2 = PS4.R2Value();
  } else {
    s.buttons = 0;
    s.lx = s.ly = s.rx = s.ry = 0;
    s.l2 = s.r2 = 0;
  }

  s.battery = readBatteryPercent();

  uint8_t f = 0;
  if (connected) {
    f |= DB_FLAG_CONNECTED;
    if (PS4.Charging()) f |= DB_FLAG_CHARGING;
    if (PS4.Audio()) f |= DB_FLAG_AUDIO;
    if (PS4.Mic()) f |= DB_FLAG_MIC;
  }
  if (motionEnabled) f |= DB_FLAG_MOTION;
  s.flags = f;
}

/**
 * Comparação com histerese: ignora variações de 1 LSB (ruído) nos
 * gatilhos e de até DB_AXIS_HYSTERESIS nos eixos. Só "acorda" o envio
 * quando algo realmente mudou.
 */
static bool inputChanged(const DbInput &a, const DbInput &b) {
  if (a.buttons != b.buttons) return true;
  if (a.flags != b.flags) return true;
  if (a.battery != b.battery) return true;

  if (abs((int)a.l2 - (int)b.l2) > 1) return true;
  if (abs((int)a.r2 - (int)b.r2) > 1) return true;

  if (abs((int)a.lx - (int)b.lx) > DB_AXIS_HYSTERESIS) return true;
  if (abs((int)a.ly - (int)b.ly) > DB_AXIS_HYSTERESIS) return true;
  if (abs((int)a.rx - (int)b.rx) > DB_AXIS_HYSTERESIS) return true;
  if (abs((int)a.ry - (int)b.ry) > DB_AXIS_HYSTERESIS) return true;

  return false;
}

static void sendInputFrame() {
  /* 11 bytes de payload -> frame total de 14 bytes:
   * header + tipo + 11 + checksum */
  uint8_t p[11];

  p[0] = ++txSeq;
  p[1] = (uint8_t)(curInput.buttons & 0xFF);
  p[2] = (uint8_t)((curInput.buttons >> 8) & 0xFF);
  p[3] = (uint8_t)(curInput.lx + 128);
  p[4] = (uint8_t)(curInput.ly + 128);
  p[5] = (uint8_t)(curInput.rx + 128);
  p[6] = (uint8_t)(curInput.ry + 128);
  p[7] = curInput.l2;
  p[8] = curInput.r2;
  p[9] = curInput.battery;
  p[10] = curInput.flags;

  writeFrame(DB_PKT_INPUT, p, sizeof(p));
}

#if DUALBIT_ENABLE_MOTION
/* Requer o parser de sensores habilitado na biblioteca PS4 (ver docs). */
static inline void putInt16LE(uint8_t *dst, int16_t value) {
  dst[0] = (uint8_t)(value & 0xFF);
  dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void sendMotionFrame() {
  uint8_t p[9];

  p[0] = ++txSeq;
  putInt16LE(&p[1], PS4.data.sensor.accelerometer.x);
  putInt16LE(&p[3], PS4.data.sensor.accelerometer.y);
  putInt16LE(&p[5], PS4.data.sensor.accelerometer.z);
  putInt16LE(&p[7], PS4.data.sensor.gyroscope.z);

  writeFrame(DB_PKT_MOTION, p, sizeof(p));
}
#endif

/* ===================================================================== */
/*                    FEEDBACK (LED / RUMBLE)                            */
/* ===================================================================== */

/**
 * Envia o output report ao DualShock 4.
 *
 * IMPORTANTE: na biblioteca aed3, setLed()/setRumble() apenas gravam os
 * valores no struct `output`; quem transmite é sendToController().
 * Enviamos somente quando algo muda (rate-limited) + refresh periódico
 * para recuperar pacotes perdidos.
 */
static void flushOutput() {
  if (!PS4.isConnected()) return;

  const uint32_t now = millis();
  const uint32_t since = now - lastOutputMs;

  if (!outputDirty && since < DB_OUTPUT_REFRESH_MS) return;
  if (outputDirty && since < DB_OUTPUT_MIN_GAP_MS) return;

  PS4.setLed(ledR, ledG, ledB);
  PS4.setRumble(rumbleSmall, rumbleLarge);

  /* Escrevemos os campos de flash diretamente: setFlashRate() recebe
   * uint8_t em ms e divide por 10, limitando a 250 ms. Assim conseguimos
   * o range completo do hardware (255 ticks = 2550 ms). */
  PS4.output.flashOn = flashOnTicks;
  PS4.output.flashOff = flashOffTicks;

  PS4.sendToController();

  lastOutputMs = now;
  outputDirty = false;
}

/* ===================================================================== */
/*                   PARSER DOS COMANDOS DO micro:bit                    */
/* ===================================================================== */

static void scheduleReboot(uint16_t delayMs) {
  uint32_t at = millis() + delayMs;
  if (at == 0) at = 1;
  rebootAt = at;
}

/**
 * Lê o MAC Bluetooth real do chip (efuse). É este o endereço que o ESP32
 * apresenta quando NÃO há override salvo — e o valor que deve ser gravado
 * no controle (sixaxispairer) se você quiser evitar o modo pareamento.
 */
static String readChipBtMac() {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return String("");
  return formatMac(mac);
}

static void startPs4() {
  if (savedMac.length() == 17) {
    if (PS4.begin(savedMac.c_str())) {
      DB_LOG("[BT] ESP32 se apresentando como %s (MAC de host)\n",
             savedMac.c_str());
      DB_LOG("[BT] O controle so conecta se tiver esse MAC memorizado.\n");
      DB_LOG("[BT] Se nao conectar, use 'pair' e pareie com PS+Share.\n");
      return;
    }
    DB_LOG("[BT] Falha ao aplicar MAC %s -> modo pareamento\n",
           savedMac.c_str());
  } else {
    const String chip = readChipBtMac();
    DB_LOG("[BT] Sem MAC de host -> pareamento aberto\n");
    DB_LOG("[BT] MAC Bluetooth do ESP32: %s\n",
           chip.length() == 17 ? chip.c_str() : "(desconhecido)");
    DB_LOG("[BT] Segure PS+Share ate a lightbar piscar 2x.\n");
  }
  /* Sem MAC: aceita o primeiro DualShock 4 que estiver pareando */
  PS4.begin();
}

static void handleHostCommand(uint8_t cmd, const uint8_t *payload,
                              uint8_t len) {
  switch (cmd) {
    /* ---------------------------------------------------------------- */
    case DB_CMD_SET_MAC: {
      if (len != 6) {
        sendAck(cmd, DB_ACK_BAD_PARAM);
        return;
      }
      savedMac = formatMac(payload);
      saveMacToNvs(savedMac);
      DB_LOG("[CFG] MAC de host definido: %s (reiniciando)\n",
             savedMac.c_str());
      sendAck(cmd, DB_ACK_REBOOTING);
      BridgeSerial.flush();
      scheduleReboot(400);
      break;
    }

    /* ---------------------------------------------------------------- */
    case DB_CMD_GET_MAC: {
      uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
      if (savedMac.length() == 17) parseMac(savedMac.c_str(), mac);
      writeFrame(DB_PKT_MAC, mac, sizeof(mac));
      break;
    }

    /* ---------------------------------------------------------------- */
    case DB_CMD_CLEAR_MAC: {
      savedMac = "";
      saveMacToNvs(savedMac);
      DB_LOG("[CFG] MAC apagado (modo pareamento). Reiniciando...\n");
      sendAck(cmd, DB_ACK_REBOOTING);
      BridgeSerial.flush();
      scheduleReboot(400);
      break;
    }

    /* ---------------------------------------------------------------- */
    case DB_CMD_RUMBLE: {
      if (len != 2) {
        sendAck(cmd, DB_ACK_BAD_PARAM);
        return;
      }
      rumbleSmall = payload[0];
      rumbleLarge = payload[1];
      outputDirty = true;
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    case DB_CMD_RUMBLE_OFF: {
      rumbleSmall = 0;
      rumbleLarge = 0;
      outputDirty = true;
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    /* ---------------------------------------------------------------- */
    case DB_CMD_LED_COLOR: {
      if (len != 3) {
        sendAck(cmd, DB_ACK_BAD_PARAM);
        return;
      }
      ledR = payload[0];
      ledG = payload[1];
      ledB = payload[2];
      flashOnTicks = 0;
      flashOffTicks = 0;
      outputDirty = true;
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    case DB_CMD_LED_FLASH: {
      if (len != 5) {
        sendAck(cmd, DB_ACK_BAD_PARAM);
        return;
      }
      ledR = payload[0];
      ledG = payload[1];
      ledB = payload[2];
      flashOnTicks = payload[3];
      flashOffTicks = payload[4];
      outputDirty = true;
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    case DB_CMD_LED_OFF: {
      ledR = ledG = ledB = 0;
      flashOnTicks = 0;
      flashOffTicks = 0;
      outputDirty = true;
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    /* ---------------------------------------------------------------- */
    case DB_CMD_SET_DEADZONE: {
      if (len != 1) {
        sendAck(cmd, DB_ACK_BAD_PARAM);
        return;
      }
      deadzone = payload[0] > 64 ? 64 : payload[0];
      saveTuningToNvs();
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    case DB_CMD_SET_HEARTBEAT: {
      if (len != 1 || payload[0] == 0) {
        sendAck(cmd, DB_ACK_BAD_PARAM);
        return;
      }
      heartbeatTicks = payload[0];
      saveTuningToNvs();
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    case DB_CMD_SET_MOTION: {
      if (len != 1) {
        sendAck(cmd, DB_ACK_BAD_PARAM);
        return;
      }
      motionEnabled = payload[0] != 0;
      saveTuningToNvs();
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    /* ---------------------------------------------------------------- */
    case DB_CMD_PING: {
      sendAck(cmd, DB_ACK_OK);
      break;
    }

    case DB_CMD_REBOOT: {
      sendAck(cmd, DB_ACK_REBOOTING);
      BridgeSerial.flush();
      scheduleReboot(150);
      break;
    }

    /* ---------------------------------------------------------------- */
    default: {
      sendAck(cmd, DB_ACK_BAD_CMD);
      break;
    }
  }
}

/**
 * Parser incremental (máquina de estados simples) do frame:
 *   [0x55][cmd][len][payload...][xor]
 */
static void hostParseByte(uint8_t byte) {
  if (hostLen == 0 && byte != DB_HOST_HEADER) return; /* ressincroniza */

  if (hostLen >= sizeof(hostBuf)) {
    hostLen = 0;
    return;
  }
  hostBuf[hostLen++] = byte;

  if (hostLen < 3) return;

  const uint8_t payloadLen = hostBuf[2];
  if (payloadLen > DB_HOST_MAX_PAYLOAD) {
    hostLen = 0;
    return;
  }

  const uint8_t frameLen = (uint8_t)(3 + payloadLen + 1);
  if (hostLen < frameLen) return;

  uint8_t cs = 0;
  for (uint8_t i = 1; i < frameLen - 1; i++) cs ^= hostBuf[i];

  if (cs == hostBuf[frameLen - 1]) {
    handleHostCommand(hostBuf[1], &hostBuf[3], payloadLen);
  } else {
    /* Rate-limit: com o micro:bit desconectado o pino RX capta ruído e
     * isso poderia inundar o console USB. */
    static uint32_t lastBadLogMs = 0;
    static uint16_t badCount = 0;
    badCount++;
    const uint32_t now = millis();
    if (now - lastBadLogMs >= 1000) {
      DB_LOG("[UART] %u frame(s) com checksum invalido (cmd 0x%02X)\n",
             (unsigned)badCount, hostBuf[1]);
      badCount = 0;
      lastBadLogMs = now;
    }
  }
  hostLen = 0;
}

/* ===================================================================== */
/*                CONSOLE DE DEBUG (USB, comandos texto)                 */
/* ===================================================================== */

static void handleDebugLine(const char *line) {
  if (strcmp(line, "help") == 0) {
    DB_LOG(
        "Comandos:\n"
        "  mac                - mostra o MAC de host salvo\n"
        "  mac=aa:bb:cc:dd:ee:ff - faz o ESP32 se apresentar com esse MAC\n"
        "  btmac              - MAC Bluetooth real do chip\n"
        "  pair               - apaga o MAC de host (pareamento aberto)\n"
        "  status             - estado atual\n"
        "  dead=N             - deadzone 0..64 (NVS)\n"
        "  hb=MS              - heartbeat em ms 10..2550 (NVS)\n"
        "  motion=0|1         - pacote de sensores (NVS)\n"
        "  led=R,G,B          - lightbar\n"
        "  rumble=SMALL,LARGE - motores 0..255\n"
        "  reboot             - reinicia\n");
    return;
  }

  if (strcmp(line, "mac") == 0) {
    DB_LOG("MAC de host salvo: %s\n",
           savedMac.length() == 17 ? savedMac.c_str() : "(nenhum)");
    DB_LOG("MAC Bluetooth do chip: %s\n", readChipBtMac().c_str());
    return;
  }

  if (strcmp(line, "btmac") == 0) {
    const String chip = readChipBtMac();
    DB_LOG("MAC Bluetooth do ESP32: %s\n",
           chip.length() == 17 ? chip.c_str() : "(desconhecido)");
    return;
  }

  if (strncmp(line, "mac=", 4) == 0) {
    uint8_t mac[6];
    if (parseMac(line + 4, mac)) {
      savedMac = formatMac(mac);
      saveMacToNvs(savedMac);
      DB_LOG("MAC de host gravado: %s (reiniciando)\n", savedMac.c_str());
      DB_LOG("O ESP32 vai se apresentar com esse endereco.\n");
      BridgeSerial.flush();
      scheduleReboot(300);
    } else {
      DB_LOG("MAC invalido. Use aa:bb:cc:dd:ee:ff\n");
    }
    return;
  }

  if (strcmp(line, "pair") == 0) {
    savedMac = "";
    saveMacToNvs(savedMac);
    DB_LOG("MAC de host apagado. Modo pareamento aberto. Reiniciando...\n");
    scheduleReboot(300);
    return;
  }

  if (strcmp(line, "reboot") == 0) {
    DB_LOG("Reiniciando...\n");
    scheduleReboot(150);
    return;
  }

  if (strcmp(line, "status") == 0) {
    DB_LOG("--- DualBit Bridge ---\n");
    DB_LOG("MAC de host..: %s\n",
           savedMac.length() == 17 ? savedMac.c_str() : "(pareamento aberto)");
    DB_LOG("MAC do chip..: %s\n", readChipBtMac().c_str());
    DB_LOG("Conectado....: %s\n", PS4.isConnected() ? "sim" : "nao");

    const uint8_t pct = readBatteryPercent();
    if (pct == DB_BATTERY_UNKNOWN) {
      DB_LOG("Bateria......: desconhecida (controle desconectado)\n");
    } else {
      DB_LOG("Bateria......: %d/10 raw -> %u%%\n", PS4.Battery(), (unsigned)pct);
    }

    DB_LOG("Deadzone.....: %u\n", (unsigned)deadzone);
    DB_LOG("Heartbeat....: %u ms\n", (unsigned)heartbeatTicks * 10u);
    DB_LOG("Motion.......: %s\n", motionEnabled ? "on" : "off");
    DB_LOG("LED..........: %u,%u,%u flash=%u/%u\n", (unsigned)ledR,
           (unsigned)ledG, (unsigned)ledB, (unsigned)flashOnTicks,
           (unsigned)flashOffTicks);
    DB_LOG("Rumble.......: small=%u large=%u\n", (unsigned)rumbleSmall,
           (unsigned)rumbleLarge);
    DB_LOG("UART.........: %d bps RX=%d TX=%d\n", DUALBIT_BAUD, DUALBIT_PIN_RX,
           DUALBIT_PIN_TX);
    return;
  }

  if (strncmp(line, "dead=", 5) == 0) {
    int v = atoi(line + 5);
    deadzone = (uint8_t)(v < 0 ? 0 : (v > 64 ? 64 : v));
    saveTuningToNvs();
    DB_LOG("Deadzone = %u\n", (unsigned)deadzone);
    return;
  }

  if (strncmp(line, "hb=", 3) == 0) {
    int ms = atoi(line + 3);
    if (ms < 10) ms = 10;
    if (ms > 2550) ms = 2550;
    heartbeatTicks = (uint8_t)(ms / 10);
    saveTuningToNvs();
    DB_LOG("Heartbeat = %u ms\n", (unsigned)heartbeatTicks * 10u);
    return;
  }

  if (strncmp(line, "motion=", 7) == 0) {
    motionEnabled = atoi(line + 7) != 0;
    saveTuningToNvs();
    DB_LOG("Motion = %s\n", motionEnabled ? "on" : "off");
    return;
  }

  if (strncmp(line, "led=", 4) == 0) {
    int r = 0, g = 0, b = 0;
    if (sscanf(line + 4, "%d,%d,%d", &r, &g, &b) == 3) {
      ledR = (uint8_t)constrain(r, 0, 255);
      ledG = (uint8_t)constrain(g, 0, 255);
      ledB = (uint8_t)constrain(b, 0, 255);
      flashOnTicks = flashOffTicks = 0;
      outputDirty = true;
      DB_LOG("LED = %u,%u,%u\n", (unsigned)ledR, (unsigned)ledG,
             (unsigned)ledB);
    } else {
      DB_LOG("Uso: led=R,G,B\n");
    }
    return;
  }

  if (strncmp(line, "rumble=", 7) == 0) {
    int s = 0, l = 0;
    if (sscanf(line + 7, "%d,%d", &s, &l) == 2) {
      rumbleSmall = (uint8_t)constrain(s, 0, 255);
      rumbleLarge = (uint8_t)constrain(l, 0, 255);
      outputDirty = true;
      DB_LOG("Rumble = small:%u large:%u\n", (unsigned)rumbleSmall,
             (unsigned)rumbleLarge);
    } else {
      DB_LOG("Uso: rumble=SMALL,LARGE\n");
    }
    return;
  }

  DB_LOG("Comando desconhecido: %s (use 'help')\n", line);
}

static void pollDebugConsole() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      dbgLine[dbgLen] = '\0';
      if (dbgLen > 0) handleDebugLine(dbgLine);
      dbgLen = 0;
    } else if (dbgLen < sizeof(dbgLine) - 1) {
      dbgLine[dbgLen++] = (char)c;
    }
  }
}

/* ===================================================================== */
/*                             SETUP / LOOP                              */
/* ===================================================================== */

void setup() {
  Serial.begin(DB_DEBUG_BAUD);
  delay(30);

  DB_LOG("\n\n=== DualBit Bridge (ESP32) ===\n");

  loadConfig();

  /* setRxBufferSize() precisa ser chamado ANTES de begin() no ESP32 */
  BridgeSerial.setRxBufferSize(256);
  BridgeSerial.begin(DUALBIT_BAUD, SERIAL_8N1, DUALBIT_PIN_RX, DUALBIT_PIN_TX);

  /* Pull-up no RX: o pino fica flutuando quando o micro:bit está
   * desconectado e o ruído gera frames inválidos. Usamos a API de pad
   * (e NÃO pinMode, que desviaria o pino da matriz da UART). */
  gpio_set_pull_mode((gpio_num_t)DUALBIT_PIN_RX, GPIO_PULLUP_ONLY);

  DB_LOG("[UART] %d bps  RX=GPIO%d  TX=GPIO%d\n", DUALBIT_BAUD, DUALBIT_PIN_RX,
         DUALBIT_PIN_TX);

  memset(&curInput, 0, sizeof(curInput));
  memset(&prevInput, 0, sizeof(prevInput));

  startPs4();

  DB_LOG("[CFG] MAC=%s deadzone=%u heartbeat=%ums motion=%s\n",
         savedMac.length() == 17 ? savedMac.c_str() : "(pareamento aberto)",
         (unsigned)deadzone, (unsigned)heartbeatTicks * 10u,
         motionEnabled ? "on" : "off");

  lastInputSentMs = millis();
}

void loop() {
  /* 1) Comandos vindos do micro:bit */
  while (BridgeSerial.available() > 0) {
    hostParseByte((uint8_t)BridgeSerial.read());
  }

  /* 2) Console de debug via USB */
  pollDebugConsole();

  /* 3) Reinício agendado (troca de MAC exige reinit do stack BT) */
  const uint32_t now = millis();
  if (rebootAt != 0 && (int32_t)(now - rebootAt) >= 0) {
    BridgeSerial.flush();
    Serial.flush();
    delay(20);
    ESP.restart();
  }

  /* 4) Snapshot do controle + envio orientado a evento/heartbeat */
  readInput(curInput);

  const uint32_t heartbeatMs = (uint32_t)heartbeatTicks * 10u;
  const bool heartbeatDue = (now - lastInputSentMs) >= heartbeatMs;

  if (inputChanged(curInput, prevInput) || heartbeatDue) {
    sendInputFrame();
    prevInput = curInput;
    lastInputSentMs = now;
  }

  /* 5) Sensores (opcional) */
#if DUALBIT_ENABLE_MOTION
  if (motionEnabled && (now - lastMotionSentMs) >= DB_MOTION_PERIOD_MS) {
    sendMotionFrame();
    lastMotionSentMs = now;
  }
#endif

  /* 6) Feedback (LED/rumble) */
  flushOutput();

  delay(2); /* ~500 Hz de polling; o BT roda em task separada */
}
