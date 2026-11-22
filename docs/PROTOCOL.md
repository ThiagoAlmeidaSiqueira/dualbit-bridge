# DualBit Bridge — Protocolo Serial Binário

Documento de referência do protocolo entre o **ESP32** e o **micro:bit**.
Fonte de verdade no código:

- `firmware/include/dualbit_protocol.h` (C/C++)
- `extension/main.ts` (TypeScript/PXT)

---

## 1. Princípios

| Item | Valor |
|---|---|
| UART | 115200 bps, 8N1, sem fluxo |
| Pinos (padrão) | ESP32 RX=GPIO16, TX=GPIO17 |
| Sentido ESP32 → micro:bit | frames iniciados por `0xAA` |
| Sentido micro:bit → ESP32 | frames iniciados por `0x55` |
| Checksum | XOR simples |
| Endianness | little-endian |
| Máximo de payload RX | 16 bytes |

O protocolo é **binário e de tamanho fixo por tipo** no sentido TX: o
micro:bit descarta o frame inteiro se o checksum falhar e ressincroniza
procurando o próximo `0xAA`.

---

## 2. TX — ESP32 → micro:bit

Formato geral:

```
┌──────┬──────┬───────────────────┬──────────┐
│ 0xAA │ tipo │ payload (N bytes) │ XOR      │
└──────┴──────┴───────────────────┴──────────┘
   [0]    [1]     [2..N+1]         [N+2]
```

`XOR` = XOR do **tipo + todo o payload** (o byte `0xAA` fica fora).

### 2.1 Tipo `0x01` — INPUT (estado do controle) — 14 bytes

```
byte  0     1     2     3     4     5     6     7     8     9    10    11    12    13
     ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
     │0xAA │0x01 │ seq │btnLo│btnHi│ lx  │ ly  │ rx  │ ry  │ l2  │ r2  │batt │flags│ XOR │
     └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
```

| Byte | Campo | Descrição |
|---|---|---|
| 0 | header | `0xAA` |
| 1 | tipo | `0x01` |
| 2 | `seq` | contador que dá a volta em 255 (detecta perda) |
| 3 | `btnLo` | botões 0–7 (bitmask) |
| 4 | `btnHi` | botões 8–15 (bitmask) |
| 5 | `lx` | analógico esq. X, `valor + 128` (0..255) |
| 6 | `ly` | analógico esq. Y, `valor + 128` (0..255) |
| 7 | `rx` | analógico dir. X, `valor + 128` (0..255) |
| 8 | `ry` | analógico dir. Y, `valor + 128` (0..255) |
| 9 | `l2` | gatilho L2 (0..255) |
| 10 | `r2` | gatilho R2 (0..255) |
| 11 | `batt` | bateria 0..100 %; `0xFF` = desconhecido |
| 12 | `flags` | status (ver abaixo) |
| 13 | checksum | XOR de `[1..12]` |

#### Máscara de botões (16 bits)

| bit | botão | bit | botão |
|---|---|---|---|
| 0 | Cross (✕) | 8 | Share |
| 1 | Circle (◯) | 9 | Options |
| 2 | Square (□) | 10 | PS |
| 3 | Triangle (△) | 11 | Touchpad (clique) |
| 4 | L1 | 12 | D-Pad ↑ |
| 5 | R1 | 13 | D-Pad ↓ |
| 6 | L3 | 14 | D-Pad ← |
| 7 | R3 | 15 | D-Pad → |

> Diagonais do D-Pad são convertidas pelo firmware em duas direções
> simultâneas (ex.: ↗ liga `Up` e `Right`).

#### Flags

| bit | máscara | Significado |
|---|---|---|
| 0 | `0x01` | controle conectado |
| 1 | `0x02` | carregando |
| 2 | `0x04` | áudio plugado |
| 3 | `0x08` | microfone plugado |
| 4 | `0x10` | pacote de sensores habilitado |

#### Eixos

- `-128` = tudo para a esquerda / para baixo
- `0` = centro (após deadzone)
- `+127` = tudo para a direita / para cima (Y já vem invertido pela lib)
- No fio vai `valor + 128`.

### 2.2 Tipo `0x02` — MOTION (opcional) — 12 bytes

```
byte  0     1     2     3   4     5   6     7   8     9    10   11
     ┌─────┬─────┬─────┬─────────┬─────────┬─────────┬─────────┬─────┐
     │0xAA │0x02 │ seq │ accX    │ accY    │ accZ    │ gyroZ   │ XOR │
     └─────┴─────┴─────┴─────────┴─────────┴─────────┴─────────┴─────┘
                        int16 LE   int16 LE   int16 LE   int16 LE
```

> **Só é enviado se** o firmware for compilado com
> `-DDUALBIT_ENABLE_MOTION=1` **e** o parser de sensores da biblioteca
> PS4 estiver habilitado (ver §6).

### 2.3 Tipo `0x03` — MAC — 9 bytes

```
┌─────┬─────┬────┬────┬────┬────┬────┬────┬─────┐
│0xAA │0x03 │ m0 │ m1 │ m2 │ m3 │ m4 │ m5 │ XOR │
└─────┴─────┴────┴────┴────┴────┴────┴────┴─────┘
```

`m0..m5` na ordem de exibição (`m0` = primeiro octeto). MAC não
configurado ⇒ todos os bytes `0x00`.

### 2.4 Tipo `0x04` — ACK — 5 bytes

```
┌─────┬─────┬────────┬────────┬─────┐
│0xAA │0x04 │ comando│resultado│ XOR │
└─────┴─────┴────────┴────────┴─────┘
```

| resultado | Significado |
|---|---|
| 0 | OK |
| 1 | comando desconhecido |
| 2 | parâmetro inválido |
| 3 | reiniciando |

---

## 3. RX — micro:bit → ESP32

Formato geral (tamanho variável, limitado a 16 bytes de payload):

```
┌─────┬────────┬─────┬──────────────────┬─────┐
│0x55 │ comando│ len │ payload (len)    │ XOR │
└─────┴────────┴─────┴──────────────────┴─────┘
  [0]     [1]    [2]     [3..len+2]      [len+3]
```

`XOR` = XOR de `comando`, `len` e do payload.

### Comandos

| Cmd | Nome | Payload | Efeito |
|---|---|---|---|
| `0x01` | `SET_MAC` | 6 bytes MAC | grava na NVS e **reinicia** |
| `0x02` | `GET_MAC` | — | responde `0x03` (MAC) |
| `0x03` | `RUMBLE` | `small`, `large` (0..255) | vibração |
| `0x04` | `LED_COLOR` | `r`, `g`, `b` | lightbar fixa |
| `0x05` | `LED_FLASH` | `r`, `g`, `b`, `on`, `off` | lightbar piscando |
| `0x06` | `LED_OFF` | — | apaga lightbar |
| `0x07` | `RUMBLE_OFF` | — | para motores |
| `0x08` | `REBOOT` | — | reinicia |
| `0x09` | `SET_DEADZONE` | 0..64 | zona morta (NVS) |
| `0x0A` | `SET_MOTION` | 0/1 | liga pacote `0x02` (NVS) |
| `0x0B` | `SET_HEARTBEAT` | 1..255 ticks | intervalo máx. (×10 ms, NVS) |
| `0x0C` | `PING` | — | responde ACK |
| `0x0D` | `CLEAR_MAC` | — | apaga MAC, modo pareamento, reinicia |

`on`/`off` do `LED_FLASH` são em **ticks de 10 ms** (255 = 2550 ms), que é
a unidade nativa do hardware do DualShock 4.

---

## 4. Exemplos prontos (para testar no serial)

### 4.1 INPUT: Cross pressionado, sticks centralizados, 100 % de bateria

```
seq=0x2A  btnLo=0x01 (bit0 = Cross)  btnHi=0x00
batt=0x64 (100 %)  flags=0x01 (conectado)
```

```
AA 01 2A 01 00 80 80 80 80 00 00 64 01 4F
```

Conferindo o XOR:

```
0x01 ^ 0x2A ^ 0x01 ^ 0x00 ^ 0x80 ^ 0x80 ^ 0x80 ^ 0x80
    ^ 0x00 ^ 0x00 ^ 0x64 ^ 0x01 = 0x4F
```

### 4.2 INPUT: nada pressionado, tudo no centro

```
AA 01 01 00 00 80 80 80 80 00 00 64 01 65
```

### 4.3 MAC `aa:bb:cc:dd:ee:ff`

```
AA 03 AA BB CC DD EE FF 12
```

### 4.4 ACK do PING

```
AA 04 0C 00 08
```

### 4.5 Comando: LED vermelho

```
55 04 03 FF 00 00 F8
```

### 4.6 Comando: rumble máximo nos dois motores

```
55 03 02 FF FF 01
```

---

## 5. Throttling e orientação a eventos

O ESP32 **não** envia tudo o que lê. A cada volta do `loop()`:

1. Lê o estado (com deadzone aplicada).
2. Se houver mudança relevante → envia na hora.
3. Se nada mudou, envia apenas 1 pacote por **heartbeat** (padrão 50 ms).

Regras de comparação:

- Botões/flags/bateria: igualdade exata.
- `L2`/`R2`: tolerância de 1 unidade (ruído).
- Eixos: tolerância de 2 unidades (histerese).

Isso mantém o tráfego típico em **~20 pacotes/s × 14 B = 280 B/s** (2,4 %
de 115200 bps), deixando o buffer do micro:bit folgado.

---

## 6. Notas de compatibilidade (importantes)

### Bateria
`PS4.Battery()` no fork `aed3/PS4-esp32` devolve `packet[42] & 0x0F`, ou
seja **0..10**. O firmware converte para `0..100` (`×10`, saturado).

### LED / vibração exigem `sendToController()`
`setLed()`, `setRumble()` e `setFlashRate()` apenas gravam no struct
`output`. Sem `PS4.sendToController()` nada chega ao controle. O firmware
chama isso quando há mudança (limitado a 1 envio a cada 20 ms) e faz um
refresh a cada 2 s.

### Giroscópio / acelerômetro
No fork `aed3`, `parsePacketSensor()` está **comentado** e
`ps4.sensor` fica com lixo. Para usar os sensores:

1. Em `src/ps4_parser.c`, descomente
   `ps4.sensor = parsePacketSensor(packet);` e o corpo da função;
2. Recompile o firmware com `-DDUALBIT_ENABLE_MOTION=1`
   (ou `#define DUALBIT_ENABLE_MOTION 1`).

Offsets do report `0x11` já usados pela lib: `lx=13`, `buttons=17`,
`l2=20`, `status=42` ⇒ sensores em `25..30` (giroscópio) e
`31..36` (acelerômetro).

### Sem BT Classic
A biblioteca usa Bluetooth **Classic**. ESP32-S2/S3/C3/C6 **não**
funcionam.

### MAC do controle
`m0..m5` aqui é o **MAC de host** salvo na NVS (o endereço que o ESP32
apresenta no Bluetooth), **não** o MAC do controle.

Motivo: na biblioteca `aed3/PS4-esp32`, `PS4.begin(mac)` chama
`esp_base_mac_addr_set()` — ou seja, muda o MAC do **próprio ESP32**. O
DualShock 4 só conecta ao host cujo endereço memorizou (normalmente o do
console PS4).

Formas de conectar:

1. **Pareamento aberto (recomendado)** — sem MAC salvo, `PS4.begin()`.
   Segure **PS + Share** no controle até a lightbar piscar duas vezes.
   Depois disso o controle memoriza o ESP32 e reconecta sozinho.
2. **"Vestir" o ESP32** com o MAC do console que o controle já conhece
   (comando `SET_MAC` / bloco *fazer o ESP32 se apresentar como*).
3. **Gravar o MAC do ESP32 no controle** com o
   [`sixaxispairer`](https://github.com/user-none/sixaxispairer). O MAC do
   chip sai no console USB com o comando `btmac`.

Nenhuma das opções acima usa o MAC do controle em si. Para apenas
**descobrir** o MAC de um controle ligado por USB no PC, use
`tools/get_ds4_mac.py` (lê o feature report `0x12`).
