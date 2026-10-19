// ENTROPYCOLLECT/1 - firmware para ESP32 classico (Arduino-ESP32).
// Wi-Fi e Bluetooth nunca sao iniciados. Nenhum hash/mistura nos dados:
// cada fonte sai BRUTA e em canal separado. SHA-256 serve so p/ integridade do USB.
// Canais: rng_hw, jitter, rtc, rom, adc, temp. Cada um sai bruto e separado.
// NAO compilado nem testado em silicio: valide numa placa real antes de distribuir.
#include <Arduino.h>
#include "mbedtls/sha256.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "soc/rtc.h"
extern "C" {
#include "bootloader_random.h"
}

const uint32_t N = 131072;       // bytes por canal em cada amostra (128 KiB)
const uint32_t RNG_GAP_US = 50;  // intervalo entre leituras do RNG (vai nos metadados)
const int ADC_PIN = 36;          // GPIO36 (VP): deixe SEM nada ligado

uint8_t gRng() {                 // RNG nativo com fonte fisica habilitada (RF desligado)
  delayMicroseconds(RNG_GAP_US);
  return esp_random() & 0xFF;
}

uint8_t gJit() {                 // jitter: bit baixo dos ciclos de CPU ate a borda do tick de 1 us
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    uint32_t c0 = ESP.getCycleCount();
    int64_t u0 = esp_timer_get_time();
    while (esp_timer_get_time() == u0) {}
    b = (b << 1) | ((ESP.getCycleCount() - c0) & 1);
  }
  return b;
}

uint8_t gRtc() {                 // CPU contra oscilador RC lento do RTC: duracao de 1 ciclo lento, em ciclos de CPU
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t r0 = rtc_time_get();
    while (rtc_time_get() == r0) {}
    uint32_t c0 = ESP.getCycleCount();
    uint64_t r1 = rtc_time_get();
    while (rtc_time_get() == r1) {}
    b = (b << 1) | ((ESP.getCycleCount() - c0) & 1);
  }
  return b;
}

uint8_t gRom() {                 // tempo de acesso a leituras da ROM (conteudo fixo; so o TEMPO varia)
  static uint32_t k = 1;
  uint32_t c0 = ESP.getCycleCount();
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) {
    k = k * 1664525u + 1013904223u;
    acc += *(volatile uint32_t*)(0x3FF90000u + ((k >> 8) & 0x3FFFu) * 4u);
  }
  uint32_t d = ESP.getCycleCount() - c0;
  if (acc == 0xFFFFFFFFu) d ^= 1;  // impede o compilador de descartar as leituras
  return d & 0xFF;
}

uint8_t gAdc() {                 // ruido do ADC em pino flutuante: bit baixo de 8 leituras
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) b = (b << 1) | (analogRead(ADC_PIN) & 1);
  return b;
}

uint8_t gTmp() {                 // sensor termico interno: byte baixo do float bruto
  float t = temperatureRead();
  uint32_t u;
  memcpy(&u, &t, 4);
  return u & 0xFF;
}

void canal(const char* nome, uint8_t (*g)()) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  Serial.printf("CH %s %u\n", nome, (unsigned)N);
  uint8_t b[64];
  char h[130];
  for (uint32_t i = 0; i < N; i += 64) {
    for (int j = 0; j < 64; j++) b[j] = g();
    mbedtls_sha256_update(&c, b, 64);
    for (int j = 0; j < 64; j++) sprintf(h + 2 * j, "%02x", b[j]);
    h[128] = '\n';
    h[129] = 0;
    Serial.print(h);
  }
  uint8_t d[32];
  mbedtls_sha256_finish(&c, d);
  mbedtls_sha256_free(&c);
  Serial.print("SHA ");
  for (int j = 0; j < 32; j++) Serial.printf("%02x", d[j]);
  Serial.println();
}

void coleta() {
  Serial.println("BEGIN");
  Serial.printf("META {\"proto\":\"ENTROPYCOLLECT/1\",\"chip\":\"%s\",\"rev\":%d,\"cpu_mhz\":%d,"
                "\"mac\":\"%012llX\",\"uptime_ms\":%lu,\"reset_reason\":%d,\"temp_c\":%.2f,"
                "\"bytes_per_channel\":%u,\"rng_gap_us\":%u,\"adc_pin\":%d}\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(), (int)getCpuFrequencyMhz(),
                ESP.getEfuseMac(), (unsigned long)millis(), (int)esp_reset_reason(),
                temperatureRead(), (unsigned)N, (unsigned)RNG_GAP_US, ADC_PIN);
  bootloader_random_enable();
  canal("rng_hw", gRng);
  bootloader_random_disable();
  canal("jitter", gJit);
  canal("rtc", gRtc);
  canal("rom", gRom);
  canal("adc", gAdc);
  canal("temp", gTmp);
  Serial.println("END");
}

void setup() { Serial.begin(921600); }

void loop() {
  Serial.println("ENTROPYCOLLECT/1 READY");
  uint32_t t = millis();
  while (millis() - t < 1000) {
    if (Serial.available() && Serial.read() == 'S') { coleta(); return; }
  }
}
