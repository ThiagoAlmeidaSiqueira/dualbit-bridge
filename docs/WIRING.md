# DualBit Bridge — Ligação física (pinagem)

> **Resumo:** os dois lados são **3,3 V**, então a ligação é direta (sem
> divisor de tensão / level shifter). O que é **obrigatório** é o **GND
> comum**.

---

## 1. Diagrama

```
   ┌──────────────────────────┐            ┌─────────────────────────┐
   │          ESP32           │            │        micro:bit        │
   │                          │            │                         │
   │  GPIO17  (TX2)  ─────────┼───────────►│  pin 2  (P2 / RX)       │
   │                          │            │                         │
   │  GPIO16  (RX2)  ◄────────┼────────────│  pin 1  (P1 / TX)       │
   │                          │            │                         │
   │  GND            ─────────┼────────────│  GND                    │
   │                          │            │                         │
   │  USB 5V (alimentação)    │            │  USB / bateria          │
   └──────────────────────────┘            └─────────────────────────┘
             │                                        │
             └────────── alimentações SEPARADAS ──────┘
                         (GND sempre em comum)
```

TX de um lado vai para RX do outro — **nunca** TX em TX.

---

## 2. Tabela de conexões

| ESP32 | micro:bit | Direção | Observação |
|---|---|---|---|
| `GPIO17` (TX2) | `pin 2` (P2) | ESP32 → micro:bit | dados de input/status |
| `GPIO16` (RX2) | `pin 1` (P1) | micro:bit → ESP32 | comandos (MAC, LED, rumble) |
| `GND` | `GND` | — | **obrigatório** |
| `USB` / `VIN 5V` | — | — | alimente o ESP32 separadamente |
| — | `USB` / bateria | — | alimente o micro:bit separadamente |

Esses pinos são os **padrões do firmware** e da extensão
(`serial.redirect(SerialPin.P1, SerialPin.P2, BaudRate.BaudRate115200)`).

Para trocar, ajuste os dois lados:

- Firmware: `#define DUALBIT_PIN_RX 16` / `#define DUALBIT_PIN_TX 17`
- Extensão: bloco **“iniciar DualBit TX … RX … baud …”**

---

## 3. Alertas (leia antes de ligar)

### 3.1 Nível lógico: 3,3 V nos dois lados
- ESP32: GPIOs em **3,3 V**.
- micro:bit: pinos do edge connector em **3,3 V**.
- Portanto **não há necessidade de divisor de tensão** nem de conversor
  de nível. Um divisor, aqui, só degrada o sinal.

### 3.2 GND comum é obrigatório
Sem GND comum os níveis de referência flutuam e a UART falha de forma
intermitente (bytes corrompidos, checksum falhando, “controle não
responde”). Se o ESP32 e o micro:bit estiverem em fontes diferentes,
ligue os GNDs **antes** dos sinais.

### 3.3 Não alimente o ESP32 pelo pino 3V do micro:bit
O pino `3V` do micro:bit não fornece corrente suficiente para o pico do
rádio Bluetooth do ESP32 (pode reiniciar ou derrubar o micro:bit).
Use USB/VIN no ESP32 e USB/bateria no micro:bit.

### 3.4 Nunca ligue 5 V nos pinos de sinal
Apesar de ambos trabalharem em 3,3 V, um `5V` vindo de USB mal ligado em
`P1`/`P2`/`GPIO16`/`GPIO17` danifica o microcontrolador. Confira o
barramento de alimentação antes de energizar.

### 3.5 GPIO16/GPIO17 em módulos WROVER
Em ESP32-WROVER os GPIO16/17 são usados pela **PSRAM**. Se o seu módulo
for WROVER, escolha outros pinos livres (ex.: `GPIO25`/`GPIO26`) e ajuste
o firmware e o bloco de inicialização.

### 3.6 Fios
- Use fios curtos (**< 20 cm**) e, de preferência, com o **GND trançado**
  junto aos dois sinais.
- 115200 bps tolera bem fios jumper comuns em bancada.
- Se houver instabilidade com fios longos, reduza o baud para 57600 nos
  dois lados ou use par trançado.

### 3.7 Conflito com a Serial do MakeCode
Depois de `serial.redirect(...)`, a `serial` do micro:bit deixa de ser o
USB: blocos como `serial.writeLine` passam a escrever **no ESP32**.
Para depuração, use `basic.showNumber`/`basic.showString` ou o console
USB do ESP32 (`Serial`, 115200).

---

## 4. Verificação rápida (checklist)

1. Firmware gravado e console USB do ESP32 mostrando
   `[UART] 115200 bps RX=GPIO16 TX=GPIO17`.
2. micro:bit com o bloco **iniciar DualBit** (TX P1 / RX P2 / 115200).
3. Sem controle conectado, o bloco **“MAC de host salvo no ESP32”**
   deve retornar `00:00:00:00:00:00` (ou o MAC já gravado) — se
   retornar vazio para sempre, inverta TX/RX ou verifique o GND.
4. Pareamento: coloque o controle em modo pareamento (**PS + Share** até
   a lightbar piscar duas vezes) → o evento **“quando o controle PS4
   conectar”** deve disparar. Depois disso o controle reconecta sozinho,
   sem precisar repetir o gesto.
5. Pressione ✕ → `isButtonPressed(Cross)` fica verdadeiro.
