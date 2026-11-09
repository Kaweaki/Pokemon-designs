# ESP32-paret: LilyGO T-Display-S3 + extern ESP32

Två ESP32-kort hopkopplade över UART. Det ena (A) sköter radion, det andra
(B, LilyGO T-Display-S3) sköter skärm, knappar och loggning.

Allt nedan är antingen **verifierat med ett kört kommando** eller märkt som
overifierat. Skillnaden står utskriven, för "kompilerar" är inte "kör" och
"kör" är inte "syns på glaset".

## Nuläge 2026-11-09

| Sak | Läge |
| --- | --- |
| LilyGO T-Display-S3 | Flashad och verifierad: ESP32-S3 rev v0.2, 16 MB, MAC 98:c3:77:a5:08:00 |
| Firmware för B | `kartograf-0.2.0`, 1 212 496 byte vid 0x10000, hash verifierad, 37,1 % flash / 34,2 % RAM |
| Boot | `Cartographer: link, store and panel lanes up` över flera resets |
| ESP32 (A) | Inte inkopplad, ingen port, ingen firmware flashad ännu |
| UART-kablage | Draget enligt tabellen nedan |
| Joystick | Inte inkopplad |

## Kablage

Båda korten pratar 3,3 V UART, **korsat**, 115200 8N1, med gemensam jord.

| Från | Till | Not |
| --- | --- | --- |
| ESP-A GPIO17 (TX) | LilyGO GPIO44 (RX) | Höger header på Lilygon, märkt 44 |
| ESP-A GPIO16 (RX) | LilyGO GPIO43 (TX) | Höger header, märkt 43 |
| ESP-A GND | LilyGO GND | Obligatoriskt |
| — | — | Koppla **aldrig** 3V3 mellan korten; varje kort matas via egen USB |

100–330 Ω i serie på varje TX-ledning rekommenderas, och håll kablarna under
~30 cm vid 115200.

Varför inte GPIO43/44 på ESP-A: på de flesta ESP32-devkort sitter
USB-seriell-bryggan där (eller på GPIO1/3 på Classic), så länken skulle slåss
med bryggan och seriemonitorn. 17/16 är en ledig andra-UART. **Undantag:** en
WROVER med PSRAM använder 16/17 — då flyttas paret till GPIO21/22.

På Lilygon är 43/44 dess egen UART-header och lediga eftersom konsolen går över
native USB.

## Joystick (4 pinnar, analog)

| Joystick | LilyGO | Not |
| --- | --- | --- |
| VCC | 3V3 (vänster header) | Aldrig 5 V — ADC:n tål max 3,3 V |
| GND | GND (vänster header) | |
| X / VRx | GPIO1 | ADC1 kanal 0, märkt ADC1_CH0 |
| Y / VRy | GPIO2 | ADC1 kanal 1, märkt ADC1_CH1 |

ADC1 är ett medvetet val: ADC2 slutar fungera när WiFi är på. Firmwaret väntar
sig viloläge kring 2048 och räknar 1200/2900 som riktning. Har joysticken en
femte pinne (SW) går den till GPIO14, som är Lilygons egen USER-knapp — den
hamnar då parallellt.

Utan joystick läser självtestets D6 flytande värden (779/265), vilket är
förväntat och inte ett fel — pinnen är oansluten.

## Panelen: GPIO15 måste sättas hög

LilyGO:s pinout kallar GPIO15 för **LCD_Power_On**. TFT_eSPI styr bakljuset
(GPIO38) men vet ingenting om den pinnen, så utan `digitalWrite(15, HIGH)`
skriver bussen till ett strömlöst glas och skärmen är svart trots att
timing-siffrorna ser bra ut. Firmwaret gör detta i `setup()` före panelinit
(`KARTOGRAF_LCD_POWER` i `board_config.h`).

**Inte visuellt verifierad**: ingen bild har tagits på skärmen. Att panelen
initieras utan krasch och fyller 320 × 170 på 27,4 ms är bevisat; att glaset
faktiskt lyser är det inte.

## Fällan som kostade mest tid

`TFT_eSPI` letar upp `<User_Setup.h>` med början i **sin egen** katalog. En kopia
i projektets `include/` når därför våra filer men aldrig `TFT_eSPI.cpp`.
Resultatet blir två olika panelkonfigurationer i samma binär — biblioteket
byggdes som ILI9341 över SPI, vår kod som 8-bitars parallell 170×320 — och
första panelskrivningen kraschade:

```
StoreProhibited, EXCVADDR 0x10
```

Beviset: backtracen pekade på `TFT_eSPI::begin_tft_write`, och disassemblyn
visade `movi.n a8, 16` följt av `s32i.n a9, a8, 0` — en lagring till absolut
adress 0x10, dvs `SPI_USER_REG(0)` när `SPI_PORT` var odefinierad.

Lösningen är bibliotekets egen PlatformIO-mekanism: `-DUSER_SETUP_LOADED=1` plus
paneldefinierna i `build_flags`, så varje översättningsenhet ser samma panel.

## Firmware

| Kort | Katalog | Roll |
| --- | --- | --- |
| B (LilyGO) | `firmware/kartografen` | Skärm, joystick, Wigle-logg, tar emot ramar på 43/44 |
| A (ESP32) | `firmware/jagaren` | Skannar, skickar AP-rapporter över UART |

```bash
pio run -d firmware/kartografen                      # bygg
pio run -d firmware/kartografen -t upload            # flasha
```

Inbyggt självtest: skriv `selftest` på USB-CDC (115200) så skrivs en
JSON-rapport mellan `SELFTEST-BEGIN` och `SELFTEST-END`.

Verifierat på hårdvara (19 kontroller): flash 16 MB, PSRAM 8192 KB, panel
170 × 320 med 27,4 ms fullskärmsfyllning, LittleFS 4096 KB med Wigle-huvud,
0 tappade UART-byte.

## Marauder: vad som faktiskt finns

Marauder har **inget** två-korts-protokoll. Enligt projektets egen wiki
exponerar varje mål en seriell CLI på 115200 — på ett kort utan skärm *är*
CLI:t gränssnittet. Det som finns i repot är skannings-, attack- och
fingeravtryckskoden.

Dessutom listas inte T-Display-S3 bland Mara uders färdiga binärer, så Marauder
på Lilygon kräver källbygge.

Tre vägar:

1. **Projektets eget par** — `jagaren` på A, `kartografen` på B. Samma protokoll
i båda ändar, ingen ny kod.
2. **Marauder på A + CLI-tolk i B** — Lilygon parsar `list -a`/`scanap` i
stället för ramar. Kräver källbygge plus en ny parser.
3. **Marauder på ett kort, utan länk** — snabbast, men då är B bara en skärm.

## Brownout

Symptom: kortet startar om mitt i en skanning, ofta just när radion sänder.
USB-porten eller kabeln orkar inte strömtopparna.

Firmwaret skriver nu orsaken som första rad efter att porten öppnats:

```
boot: reset reason 9 (BROWNOUT)
```

Siffrorna kommer från `esp_reset_reason()`. **På den här Lilygon-konfigurationen
är 11 normalt** (`ESP_RST_USB`, native USB-Serial/JTAG) — en vanlig omstart
rapporterar alltså 11, inte 1. Bara 9 betyder brownout, och 4 betyder panik.

Skrivs ingen rad alls kan strömmen falla så snabbt att USB-CDC hinner dö; då
syns det i stället som att kortet "startar om utan anledning".

Åtgärder, i ordning:

1. Annan USB-port (baktill på stationär dator, eller en port med egen
strömförsörjning) och en kort, tjock kabel — inte en laddkabel.
2. 470 µF elektrolyt över 3V3/GND nära kortet, plus 10 µF keramisk och 100 nF
vid varje modul. Det tar topparna som radio-burstarna ger.
3. Sänkt sändningseffekt i firmware (`WiFi.setTxPower(WIFI_POWER_8_5dBm)`) om
räckvidden inte är kritisk.
4. Mät 3V3 under skanning. Under ~3,0 V vid modulen är det strömförsörjningen och
inget annat.

Ingen brownout har observerats på Lilygon: sedan flashningen har varje boot
rapporterat 11, och inga `Brownout detector was triggered` finns i loggen.

## Inte verifierat

- Hur skärmen **ser ut** — ingen bild har tagits; bara busstider och att ingen
krasch sker.
- UART-länken mot A — A har varit oinkopplad hela tiden, så noll ramar har
tagits emot.
- Joysticken — inte inkopplad; D6 läser därför flytande värden i stället för
~2048.
