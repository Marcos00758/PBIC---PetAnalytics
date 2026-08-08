#include <SD.h>
#include <SPI.h>

// ── Pinos SPI externos do Nicla Voice ─────────────────────────────
// CS   = D6
// MOSI = D8
// MISO = D7
// SCK  = D9

#define SD_CS 6

File myFile;

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("Inicializando SD...");

  // Inicializa SD
  if (!SD.begin(SD_CS)) {
    Serial.println("Falha ao inicializar SD!");
    while (1)
      ;
  }

  Serial.println("SD inicializado com sucesso!");

  // ── Criar pasta ───────────────────────────────────────────────
  if (SD.mkdir("/dados")) {
    Serial.println("Pasta criada!");
  } else {
    Serial.println("Pasta ja existe ou erro.");
  }

  // ── Criar arquivo e escrever ──────────────────────────────────
  myFile = SD.open("/dados/teste.txt", FILE_WRITE);

  if (myFile) {
    myFile.println("Nicla Voice funcionando!");
    myFile.println("Teste de escrita no SD.");
    myFile.close();

    Serial.println("Arquivo salvo com sucesso.");
  } else {
    Serial.println("Erro ao abrir arquivo.");
  }

  // ── Ler o arquivo ─────────────────────────────────────────────
  myFile = SD.open("/dados/teste.txt");

  if (myFile) {
    Serial.println("Conteudo do arquivo:");

    while (myFile.available()) {
      Serial.write(myFile.read());
    }

    myFile.close();
  } else {
    Serial.println("Erro ao ler arquivo.");
  }
}

void loop() {}