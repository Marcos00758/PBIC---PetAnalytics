// ── Diagnóstico I2C + SD para o protótipo da coleira (Nicla Voice) ───────────
// Sketch INDEPENDENTE do principal. NÃO grava nada, NÃO trava: fica varrendo o
// barramento em loop para você mexer nas conexões e ver o resultado ao vivo.
//
// Hardware esperado:
//   PCA9548A (mux) @ 0x70  |  SD no J1 (CS=6)
//   ICM-20948 (0x68/0x69, WHO_AM_I reg0x00=0xEA) → canais 0, 1, 4
//   BMP390   (0x76/0x77, CHIP_ID  reg0x00=0x60) → canais 2, 3
//
// Sobe, abre o Serial Monitor a 115200 e observe. Pode mexer nos JST enquanto roda.

#include "Nicla_System.h"
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#define PCA_ADDR 0x70
#define SD_CS_PIN 6

void pca_select(uint8_t ch) {
  Wire.beginTransmission(PCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();
  delayMicroseconds(80);
}
void pca_deselect() {
  Wire.beginTransmission(PCA_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
}

// Lê 1 registrador de um endereço (canal já selecionado). 0xFF = sem resposta.
uint8_t read_reg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return 0xFF;
  Wire.requestFrom(addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// Identifica o que respondeu num endereço (canal já selecionado).
void identify(uint8_t addr) {
  Serial.print(" 0x");
  Serial.print(addr, HEX);
  if (addr == PCA_ADDR) {
    Serial.print("(mux)");
    return;
  }
  if (addr == 0x68 || addr == 0x69) {
    uint8_t who = read_reg(addr, 0x00); // WHO_AM_I
    Serial.print(who == 0xEA ? "(ICM-20948 OK)" : "(ICM? WHO=0x");
    if (who != 0xEA) {
      Serial.print(who, HEX);
      Serial.print(")");
    }
    return;
  }
  if (addr == 0x76 || addr == 0x77) {
    uint8_t id = read_reg(addr, 0x00); // CHIP_ID
    Serial.print(id == 0x60 ? "(BMP390 OK)" : "(BMP? ID=0x");
    if (id != 0x60) {
      Serial.print(id, HEX);
      Serial.print(")");
    }
    return;
  }
  Serial.print("(?)");
}

void scan_all() {
  for (uint8_t ch = 0; ch < 8; ch++) {
    pca_select(ch);
    Serial.print("  canal ");
    Serial.print(ch);
    Serial.print(":");
    bool any = false;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      if (a == PCA_ADDR)
        continue; // o mux aparece em todo canal — pula p/ não poluir
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        identify(a);
        any = true;
      }
    }
    Serial.println(any ? "" : " (vazio)");
  }
  pca_deselect();
}

void setup() {
  nicla::begin();
  nicla::leds.begin();
  nicla::leds.setColor(blue);

  Serial.begin(115200);
  delay(3000); // mesmo padrão do sketch principal (sem while(!Serial))

  Serial.println();
  Serial.println("=== DIAGNOSTICO I2C + SD (Nicla Voice) ===");

  // ── SD (J1, CS=6) ─────────────────────────────────────────────────────────
  Serial.print("SD.begin(6): ");
  if (SD.begin(SD_CS_PIN))
    Serial.println("OK");
  else
    Serial.println("FALHOU (verificar cartao/contatos)");

  // ── Wire / mux ────────────────────────────────────────────────────────────
  Wire.begin();
  Wire.setClock(400000);
  delay(50);

  Wire.beginTransmission(PCA_ADDR);
  bool mux = (Wire.endTransmission() == 0);
  Serial.print("PCA9548A @0x70: ");
  Serial.println(mux ? "OK" : "NAO RESPONDE (mux/alimentacao/contato)");

  Serial.println("Esperado: ICM(0x68/0x69) nos canais 0,1,4 | BMP390(0x76/0x77) nos canais 2,3");
  Serial.println("Pode mexer nos conectores — o scan repete a cada ~1.5s.");
  Serial.println();
}

void loop() {
  static uint32_t n = 0;
  nicla::leds.setColor((n++ & 1) ? green : blue); // pisca = está vivo

  Serial.print("--- scan #");
  Serial.print(n);
  Serial.println(" ---");
  scan_all();
  Serial.println();
  delay(1500);
}
