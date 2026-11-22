# DualBit Bridge

Ponte **Bluetooth ↔ Serial** entre um controle de **PlayStation 4
(DualShock 4)** e um **BBC micro:bit**, usando um **ESP32** no meio.

```
┌──────────────┐   Bluetooth Classic   ┌──────────────┐   UART 115200   ┌──────────────┐
│ DualShock 4  │ ◄───────────────────► │    ESP32     │ ◄─────────────► │  micro:bit   │
└──────────────┘      (HID/L2CAP)      └──────────────┘  3V3, GND comum └──────────────┘
                                            │                                 │
                                       PS4Controller                      Extensão PXT
                                      + Preferences(NVS)                 (blocos MakeCode)
```

O ESP32 lê o controle e manda um **pacote binário de tamanho fixo** para o
micro:bit (nada de JSON/strings), e recebe comandos curtos para
**vibração** e **lightbar**.

## Repositórios

| Repositório | Conteúdo |
|---|---|
| [`dualbit-bridge`](https://github.com/ThiagoAlmeidaSiqueira/dualbit-bridge) | monorepo: firmware, extensão, docs e ferramentas |
| [`pxt-dualbit-bridge`](https://github.com/ThiagoAlmeidaSiqueira/pxt-dualbit-bridge) | **apenas a extensão**, com `pxt.json` na raiz — é esta URL que você cola no MakeCode |

---

## Estrutura do repositório

```
BitPad/
├── firmware/                     # ESP32 (C++ / Arduino / PlatformIO)
│   ├── platformio.ini
│   ├── include/
│   │   └── dualbit_protocol.h    # constantes do protocolo (fonte de verdade)
│   └── src/
│       └── main.cpp              # firmware completo e comentado
├── extension/                    # Extensão PXT (MakeCode / TypeScript)
│   ├── pxt.json
│   ├── enums.ts
│   ├── main.ts
│   └── README.md                 # documentação dos blocos (usada pelo MakeCode)
├── docs/
│   ├── PROTOCOL.md               # diagrama de bytes TX/RX + exemplos
│   └── WIRING.md                 # pinagem + alertas elétricos
├── tools/
│   └── get_ds4_mac.py            # lê o MAC Bluetooth do controle via USB
└── README.md
```

---

## 1. Firmware do ESP32

### 1.1 Pré-requisitos

- ESP32 **clássico** (precisa de **Bluetooth Classic** — não serve
  S2/S3/C3/C6).
- [PlatformIO](https://platformio.org/) (VS Code ou CLI).

### 1.2 Compilar e gravar

```bash
cd firmware
pio run -t upload
pio device monitor        # 115200 baud
```

Dependências (já no `platformio.ini`):

```ini
platform = espressif32@6.9.0
lib_deps  = https://github.com/aed3/PS4-esp32.git
```

### 1.3 O que o firmware faz

| Recurso | Detalhe |
|---|---|
| Persistência | MAC do controle em **NVS** (`Preferences`, namespace `dualbit`) |
| Entrada | 16 botões digitais, L2/R2, 4 eixos, bateria, flags de status |
| Saída | Rumble (small/large), lightbar RGB, flash |
| UART | `UART2`, **115200 8N1**, RX=GPIO16, TX=GPIO17 |
| Framing | `0xAA` + tipo + payload + **XOR** |
| Throttling | envio **por evento** + **heartbeat** de 50 ms (20 Hz) |
| Deadzone | configurável (0–64), persistida, com histerese de 2 LSB |
| Console | comandos de texto na USB (`help`) |

### 1.4 Console de debug (USB, 115200)

```
mac                       → mostra o MAC de host salvo + MAC do chip
mac=aa:bb:cc:dd:ee:ff     → faz o ESP32 se apresentar com esse MAC
btmac                     → MAC Bluetooth real do chip (efuse)
pair                      → apaga o MAC de host (pareamento aberto)
status                    → estado completo
dead=8                    → zona morta
hb=50                     → heartbeat em ms
motion=0|1                → pacote de sensores
led=255,0,0               → lightbar
rumble=120,255            → motor fraco, motor forte
reboot                    → reinicia
```

### 1.5 Pareamento do controle

O DualShock 4 **só conecta ao host cujo MAC ele memorizou**. Existem três
caminhos:

| Caminho | Como | Quando usar |
|---|---|---|
| **A. Pareamento aberto** ⭐ | Sem MAC salvo, segure **PS + Share** no controle até a lightbar piscar 2× | Uso normal. Depois disso o controle reconecta sozinho |
| **B. "Vestir" o ESP32** | `mac=<MAC do console PS4>` no console (ou bloco *fazer o ESP32 se apresentar como*) | Você sabe o MAC do console e quer conexão imediata |
| **C. Gravar o MAC no controle** | `sixaxispairer` com o MAC do chip (`btmac` no console) | Sem console PS4 por perto |

> ⚠️ `PS4.begin(mac)` **não** grava o MAC do controle: ela chama
> `esp_base_mac_addr_set()` e muda o MAC do **próprio ESP32**.

### 1.6 Descobrir o MAC de um controle ligado por USB

Se o controle estiver no PC por cabo, o endereço Bluetooth está no
*feature report* `0x12` exposto pelo próprio controle:

```bash
python tools/get_ds4_mac.py
```

O script fala direto com o driver `libusb0` (libusb-win32), sem alterar
nada no sistema. Saída real de um CUH-ZCT2:

```
Controle : DualShock 4 v2 (CUH-ZCT2)
USB ID   : 054c:09cc
  Bruto    : 12 5A A0 A4 C1 BC D0 08 25 00 9B 1D 82 EB 27 B8
  MAC Bluetooth (DS4 no ESP32): d0:bc:c1:a4:a0:5a
  MAC do lado USB            : 82:1d:9b:00:25:08
```

> Esse MAC é útil para ferramentas como `sixaxispairer` e para
> identificar o controle. Ele **não** é o valor usado em `PS4.begin()`.

---

## 2. Extensão MakeCode (PXT)

### 2.1 Importar no MakeCode

A extensão está publicada em um repositório próprio, com o `pxt.json` na
raiz (exigência do MakeCode):

**https://github.com/ThiagoAlmeidaSiqueira/pxt-dualbit-bridge**

No MakeCode (micro:bit): **Extensões → GitHub** → cole a URL acima.

O monorepo deste projeto fica em:
**https://github.com/ThiagoAlmeidaSiqueira/dualbit-bridge**

> O repositório da extensão é derivado da pasta `extension/` via
> `git subtree split`. Para re-sincronizar depois de alterar a extensão:
>
> ```bash
> git subtree split --prefix=extension -b pxt-publish
> git push pxt pxt-publish:main --force
> ```

**Opção B — editor local**

1. Clone o repositório do MakeCode (`makecode.microbit.org` → *Explore*).
2. Copie a pasta `extension/` para `libs/dualbit-bridge/`.
3. Referencie no `pxt.json` da aplicação:

   ```json
   "dependencies": { "dualbit-bridge": "file:../libs/dualbit-bridge" }
   ```

4. `pxt serve`.

### 2.2 Exemplo de programa

```blocks
DualBit.startDefault()

DualBit.onPS4Connected(function () {
    basic.showIcon(IconNames.Happy)
    DualBit.setLedRgb(0, 64, 255)
})

DualBit.onPS4Disconnected(function () {
    basic.showIcon(IconNames.Sad)
})

DualBit.onButtonPressed(DualBitButton.Cross, function () {
    DualBit.setLedColor(0x00FF00)
    DualBit.rumblePulse(DualBitMotor.Both, 100, 250)
})

DualBit.onButtonReleased(DualBitButton.Cross, function () {
    DualBit.setLedColor(0x0040FF)
})

basic.forever(function () {
    // motores do micro:bit seguindo o analógico esquerdo (-1023..1023)
    let y = DualBit.axisScaled(DualBitStick.Left, DualBitAxis.Y, DualBitScale.Pwm1023)
    if (y > 120) {
        pins.digitalWritePin(DigitalPin.P8, 1)
    } else {
        pins.digitalWritePin(DigitalPin.P8, 0)
    }
})
```

### 2.3 Blocos disponíveis

| Grupo | Blocos |
|---|---|
| Configuração | iniciar DualBit (TX/RX/baud), iniciar padrão |
| Conexão | onPS4Connected, onPS4Disconnected, isPS4Connected, setMac, macAddress, refreshMac, onMacAddressReceived |
| Eventos | onButtonPressed, onButtonReleased, onBatteryChanged |
| Leitura | isButtonPressed, trigger L2/R2, axis (bruto), axisScaled (%/PWM), batteryLevel |
| Feedback | setLedColor, setLedRgb, setLedFlash, ledOff, setRumble, rumblePulse, rumbleOff, stopFeedback |
| Avançado | enableMotion, acceleration, rotationZ, setDeadzone, setHeartbeat, rebootEsp32, clearMacAddress, ping, lastAckResult, onAck |

---

## 3. Ligação física (resumo)

| ESP32 | micro:bit |
|---|---|
| `GPIO17` (TX2) | `pin 2` (P2 / RX) |
| `GPIO16` (RX2) | `pin 1` (P1 / TX) |
| `GND` | `GND` |

- **3,3 V nos dois lados** → ligação direta, **sem divisor de tensão**.
- **GND comum é obrigatório.**
- **Não** alimente o ESP32 pelo pino `3V` do micro:bit.
- Detalhes e alertas: [`docs/WIRING.md`](docs/WIRING.md).

---

## 4. Protocolo (resumo)

```
TX  (ESP32 → micro:bit)            RX  (micro:bit → ESP32)
┌──────┬──────┬─────────┬─────┐    ┌─────┬─────┬─────┬─────────┬─────┐
│ 0xAA │ tipo │ payload │ XOR │    │0x55 │ cmd │ len │ payload │ XOR │
└──────┴──────┴─────────┴─────┘    └─────┴─────┴─────┴─────────┴─────┘
```

| Tipo TX | Tam. | Conteúdo |
|---|---|---|
| `0x01` | 14 B | seq, 16 botões (2 B), 4 eixos, L2, R2, bateria, flags |
| `0x02` | 12 B | accel XYZ + giro Z (opcional) |
| `0x03` | 9 B | MAC salvo (6 B) |
| `0x04` | 5 B | ACK de comando |

Diagramas de bytes completos, máscaras de bits, tabela de comandos e
exemplos prontos para testar: [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

---

## 5. Notas de implementação

- `PS4.setLed()`/`setRumble()` só gravam o struct; o envio real é feito
  por **`PS4.sendToController()`** (chamado com throttle de 20 ms e
  refresh de 2 s).
- `PS4.Battery()` no fork `aed3` devolve **0..10** → o firmware converte
  para `0..100`.
- O parser de sensores do fork está **desativado**; `DUALBIT_ENABLE_MOTION`
  exige um patch na biblioteca (instruções em `docs/PROTOCOL.md` §6).
- No micro:bit, todos os buffers são estáticos e a leitura usa
  `serial.readBuffer(1)` (bloqueante, dorme a fibra) + `readBuffer(0)`
  (drena o que chegou), evitando *busy-wait* e o erro 020.

## Licença

MIT.
