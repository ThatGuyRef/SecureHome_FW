#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include "rahasia.h"

#define PIN_SS     5
#define PIN_RST    22
#define PIN_SPK    4
#define PIN_PIR    27
#define PIN_PINTU  14
#define PIN_KAMERA 32

#define PULSA_BUZZER   100
#define DURASI_IZIN    20000
#define JEDA_PIR_SIAP  5000

#define PICU_PERLU     1
#define JENDELA_PICU   15000
#define JEDA_ANTAR_PICU 3000

#define POTONGAN_SIRENE 60
#define JEDA_SEGAR_KARTU 30000
#define KUNCI_DAFTAR     60000   // abaikan perintah daftar selama 1 menit sesudah sukses

MFRC522 rfid(PIN_SS, PIN_RST);

String token, ownerId, mode = "?";
String kartu[40], namaKartu[40];
int    jumlahKartu = 0;
String tandaKartu = "";

bool alarmAktif = false;
bool modeDaftar = false;
unsigned long abaikanDaftarSampai = 0;
int  fSirene = 2300;
unsigned long tIzin = 0;

unsigned long tDetak = 0, tState = 0, tKartu = 0, tLogin = 0, tRfid = 0;
unsigned long tAlertPir = 0, tAlertPintu = 0;
bool pernahAlertPir = false, pernahAlertPintu = false;
const unsigned long JEDA_ALERT = 60000;

int  pintuStabil = -1, pintuBaca = -1;
unsigned long tPintu = 0;

bool pirTerakhir = false;
unsigned long tSiapPir = 0;
int  picuHitung = 0;
unsigned long tPicuPertama = 0, tPicuTerakhir = 0;

// ================= jaringan =================
static bool httpKirim(const char* metode, const String& path,
                      const String& body, String& out, int& code) {
  for (int coba = 1; coba <= 3; coba++) {
    WiFiClientSecure cli;
    cli.setInsecure();
    cli.setTimeout(20);
    cli.setHandshakeTimeout(20);

    HTTPClient http;
    http.setTimeout(20000);
    http.setReuse(false);

    if (http.begin(cli, String(SB_URL) + path)) {
      http.addHeader("apikey", SB_ANON);
      if (token.length()) http.addHeader("Authorization", "Bearer " + token);
      if (body.length())  http.addHeader("Content-Type", "application/json");

      code = http.sendRequest(metode, (uint8_t*)body.c_str(), body.length());
      out  = http.getString();
      http.end();
      if (code > 0) return true;
    } else code = -100;

    delay(700);
  }
  return false;
}

int sbGet(const String& p, String& out) { int c; httpKirim("GET", p, "", out, c); return c; }
int sbPost(const String& p, const String& b, String& out) { int c; httpKirim("POST", p, b, out, c); return c; }

bool sbLogin() {
  String body = String("{\"email\":\"") + DEV_EMAIL +
                "\",\"password\":\"" + DEV_PASS + "\"}";
  for (int i = 1; i <= 3; i++) {
    token = "";
    String out; int code;
    httpKirim("POST", "/auth/v1/token?grant_type=password", body, out, code);
    if (code == 200) {
      JsonDocument doc; deserializeJson(doc, out);
      token = doc["access_token"].as<String>();
      tLogin = millis();
      Serial.printf("Login perangkat OK (heap %u)\n", ESP.getFreeHeap());
      return true;
    }
    Serial.printf("Login gagal percobaan %d, kode %d %s\n", i, code, out.c_str());
    delay(2000);
  }
  return false;
}

// ================= Supabase =================
void sbDetak() {
  if (!token.length()) return;
  String out;
  int c = sbPost("/rest/v1/rpc/touch_device",
                 String("{\"p_key\":\"") + DEVICE_KEY + "\"}", out);
  if (c == 401) { token = ""; return; }
  if (c != 200 && c != 204) Serial.printf("detak gagal %d %s\n", c, out.c_str());
}

void sbAmbilPemilik() {
  if (!token.length()) return;
  String out;
  int c = sbGet(String("/rest/v1/devices?select=user_id&device_key=eq.") + DEVICE_KEY, out);
  if (c != 200) { Serial.printf("pemilik gagal %d %s\n", c, out.c_str()); return; }
  JsonDocument doc; deserializeJson(doc, out);
  if (doc.size() > 0) ownerId = doc[0]["user_id"].as<String>();
  Serial.println("Pemilik: " + ownerId);
}

void sbTutupPendaftaran() {
  if (!token.length() || !ownerId.length()) return;
  String out; int c;
  httpKirim("PATCH", "/rest/v1/device_state?user_id=eq." + ownerId,
            "{\"enroll_until\":null}", out, c);
  if (c >= 300 || c < 0)
    Serial.printf("tutup pendaftaran GAGAL %d %s\n", c, out.c_str());
  else
    Serial.println("jendela pendaftaran ditutup di server");
}

void sbAmbilStatus() {
  if (!token.length()) return;
  String out;
  int c = sbPost("/rest/v1/rpc/status_perangkat", "{}", out);
  if (c != 200) return;
  JsonDocument doc; deserializeJson(doc, out);

  String m = doc["mode"].as<String>();
  if (m.length() && m != mode) { mode = m; Serial.println("MODE = " + mode); }

  bool d = doc["daftar"] | false;
  if (d && abaikanDaftarSampai && millis() < abaikanDaftarSampai) {
    d = false;                                   // baru saja mendaftarkan kartu
  }
  if (d != modeDaftar) {
    modeDaftar = d;
    Serial.println(modeDaftar ? ">>> MODE PENDAFTARAN AKTIF - tempelkan kartu baru"
                              : ">>> mode pendaftaran selesai");
  }
}

void sbAmbilKartu() {
  if (!token.length()) return;
  String out;
  if (sbGet("/rest/v1/nfc_cards?select=uid,name", out) != 200) return;
  JsonDocument doc; deserializeJson(doc, out);

  jumlahKartu = 0;
  String tanda = "";
  for (JsonObject o : doc.as<JsonArray>()) {
    if (jumlahKartu >= 40) break;
    String u = o["uid"].as<String>();
    u.replace(" ", ""); u.toUpperCase();
    kartu[jumlahKartu]     = u;
    namaKartu[jumlahKartu] = o["name"].as<String>();
    tanda += u + ",";
    jumlahKartu++;
  }

  if (tanda != tandaKartu) {
    tandaKartu = tanda;
    Serial.printf("Daftar kartu diperbarui: %d kartu\n", jumlahKartu);
    for (int i = 0; i < jumlahKartu; i++)
      Serial.println("  - " + kartu[i] + "  (" + namaKartu[i] + ")");
  }
}

void sbCatat(const char* tone, const char* glyph,
             const String& judul, const String& isi) {
  if (!token.length() || !ownerId.length()) return;
  JsonDocument d;
  d["user_id"]     = ownerId;
  d["tone"]        = tone;
  d["kind"]        = "alert";
  d["glyph"]       = glyph;
  d["title"]       = judul;
  d["description"] = isi;
  d["unread"]      = true;
  String body; serializeJson(d, body);
  String out;
  int c = sbPost("/rest/v1/alerts", body, out);
  if (c >= 300) Serial.printf("catat GAGAL %d %s\n", c, out.c_str());
  else          Serial.println("tercatat: " + judul);
}

bool sbDaftarkanKartu(const String& uid) {
  if (!token.length() || !ownerId.length()) return false;
  JsonDocument d;
  d["user_id"] = ownerId;
  d["name"]    = "Kartu baru";
  d["uid"]     = uid;
  String body; serializeJson(d, body);
  String out;
  int c = sbPost("/rest/v1/nfc_cards", body, out);
  if (c >= 300) { Serial.printf("daftar GAGAL %d %s\n", c, out.c_str()); return false; }
  Serial.println("KARTU BARU TERDAFTAR: " + uid);
  return true;
}

// ================= suara & alarm =================
void nada(int freq, int ms) {
  int periode = 1000000 / freq;
  int pulsa   = PULSA_BUZZER;
  if (pulsa > periode / 2) pulsa = periode / 2;
  long n = (long)ms * 1000L / periode;
  for (long i = 0; i < n; i++) {
    digitalWrite(PIN_SPK, HIGH); delayMicroseconds(pulsa);
    digitalWrite(PIN_SPK, LOW);  delayMicroseconds(periode - pulsa);
  }
  digitalWrite(PIN_SPK, LOW);
}

void potonganSirene() {
  static int hitung = 0;
  nada(fSirene, POTONGAN_SIRENE);
  if (++hitung >= 6) { hitung = 0; fSirene = (fSirene == 2300) ? 2800 : 2300; }
}

void bipSah()     { nada(2600, 90); }
void bipMasuk()   { nada(2200, 100); delay(60); nada(2700, 160); }
void bipDaftar()  { nada(2400, 80); delay(50); nada(2400, 80); delay(50); nada(2900, 200); }
void bipGagal()   { nada(1200, 250); }

bool sedangAktif() { return mode != "off" && mode != "nonaktif" && mode != "?"; }
bool modeAway()    { return mode == "away" || mode == "pergi"; }

void nyalakanAlarm(const char* sebab) {
  if (!sedangAktif()) {
    Serial.printf("(mode nonaktif - alarm diabaikan: %s)\n", sebab);
    return;
  }
  if (!alarmAktif) {
    alarmAktif = true;
    nada(2800, 200); nada(2300, 200);
    Serial.printf(">>> ALARM MENYALA (%s)\n", sebab);
    Serial.println("    matikan dengan kartu terdaftar atau lewat aplikasi");
  }
}

void matikanAlarm() { alarmAktif = false; digitalWrite(PIN_SPK, LOW); }

bool sedangDiizinkan() { return tIzin && (millis() - tIzin < DURASI_IZIN); }

// ================= perangkat keras =================
void picuKamera() {
  digitalWrite(PIN_KAMERA, LOW);
  delay(200);
  digitalWrite(PIN_KAMERA, HIGH);
}

String bacaUid() {
  String s = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) s += "0";
    s += String(rfid.uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void tanganiKartu() {
  if (!rfid.PICC_IsNewCardPresent() && !rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uid = bacaUid();
  int cocok = -1;
  for (int i = 0; i < jumlahKartu; i++)
    if (uid.equalsIgnoreCase(kartu[i])) { cocok = i; break; }

  if (cocok >= 0) {
    bool tadinyaAlarm = alarmAktif;
    matikanAlarm();
    tIzin = millis();
    picuHitung = 0;
    Serial.println("DITERIMA: " + namaKartu[cocok] + " (" + uid + ")");
    if (tadinyaAlarm) Serial.println(">>> ALARM DIMATIKAN");
    bipMasuk();
    sbCatat("success", "check",
            tadinyaAlarm ? "Alarm dimatikan" : "Akses diterima",
            namaKartu[cocok] + (tadinyaAlarm ? " mematikan alarm" : " membuka pintu"));

  } else if (modeDaftar) {
    Serial.println("KARTU BARU: " + uid);
    if (sbDaftarkanKartu(uid)) {
      modeDaftar = false;
      abaikanDaftarSampai = millis() + KUNCI_DAFTAR;
      bipDaftar();
      sbTutupPendaftaran();          // tutup jendela di server
      sbAmbilKartu();
      sbCatat("success", "check", "Kartu baru terdaftar",
              "Kartu " + uid + " didaftarkan lewat aplikasi");
      Serial.println(">>> mode pendaftaran selesai");
    } else bipGagal();

  } else {
    Serial.printf("DITOLAK: %s (tidak cocok dengan %d kartu terdaftar)\n",
                  uid.c_str(), jumlahKartu);
    if (sedangAktif()) {
      nyalakanAlarm("kartu tidak dikenal");
      picuKamera();
      sbCatat("danger", "warning", "Kartu tidak dikenal",
              "Ada yang menempelkan kartu asing di pintu depan");
    } else {
      Serial.println("  mode nonaktif - hanya dicatat, alarm tidak menyala");
      bipGagal();
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void tanganiPintu() {
  int v = digitalRead(PIN_PINTU);
  if (v != pintuBaca) { pintuBaca = v; tPintu = millis(); }
  if (pintuBaca == pintuStabil || millis() - tPintu <= 50) return;
  pintuStabil = pintuBaca;

  if (!sedangAktif()) {
    Serial.println(pintuStabil == HIGH
                   ? "PINTU TERBUKA (mode nonaktif - diabaikan)"
                   : "PINTU TERTUTUP (mode nonaktif - diabaikan)");
    return;
  }

  if (pintuStabil == HIGH) {
    if (sedangDiizinkan()) {
      Serial.println("PINTU TERBUKA - akses sah");
      bipSah();
      sbCatat("success", "door", "Pintu dibuka",
              "Pintu dibuka setelah kartu terdaftar ditempelkan");
    } else {
      Serial.println("PINTU TERBUKA / KABEL PUTUS - tidak sah");
      nyalakanAlarm("pintu dibuka tanpa kartu, atau kabel sensor diputus");
      picuKamera();
      if (!pernahAlertPintu || millis() - tAlertPintu > JEDA_ALERT) {
        pernahAlertPintu = true;
        tAlertPintu = millis();
        sbCatat("danger", "door", "Pintu dibuka paksa",
                "Pintu depan dibuka tanpa kartu terdaftar");
      }
    }
  } else {
    Serial.println("PINTU TERTUTUP");
    if (sedangDiizinkan()) bipSah();
  }
}

void tanganiPir() {
  bool v = digitalRead(PIN_PIR);
  if (v != pirTerakhir) {
    Serial.printf("PIR %s\n", v ? "HIGH" : "low");
    pirTerakhir = v;
  }
  if (!v) return;

  if (!modeAway()) return;

  unsigned long now = millis();
  if (now < tSiapPir)    return;
  if (sedangDiizinkan()) return;
  if (tPicuTerakhir && now - tPicuTerakhir < JEDA_ANTAR_PICU) return;
  tPicuTerakhir = now;

  if (picuHitung == 0 || now - tPicuPertama > JENDELA_PICU) {
    picuHitung   = 1;
    tPicuPertama = now;
  } else picuHitung++;

  Serial.printf("  gerakan ke-%d dari %d yang dibutuhkan\n", picuHitung, PICU_PERLU);
  if (picuHitung < PICU_PERLU) { picuKamera(); return; }
  picuHitung = 0;

  nyalakanAlarm("gerakan terdeteksi saat mode away");
  picuKamera();

  if (!pernahAlertPir || now - tAlertPir > JEDA_ALERT) {
    pernahAlertPir = true;
    tAlertPir = now;
    sbCatat("danger", "motion", "Penyusup terdeteksi",
            "Gerakan terdeteksi saat mode away - alarm menyala");
  }
}

void scanCepat() {
  tanganiPintu();
  tanganiPir();
  if (alarmAktif) potonganSirene();
}

// ================= setup & loop =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SecureHome papan 1 ===");

  pinMode(PIN_SPK, OUTPUT);    digitalWrite(PIN_SPK, LOW);
  pinMode(PIN_KAMERA, OUTPUT); digitalWrite(PIN_KAMERA, HIGH);
  pinMode(PIN_PIR, INPUT_PULLDOWN);
  pinMode(PIN_PINTU, INPUT_PULLUP);

  SPI.begin(18, 19, 23, PIN_SS);
  rfid.PCD_Init();
  delay(50);
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  Serial.printf("RC522 VersionReg = 0x%02X\n",
                rfid.PCD_ReadRegister(MFRC522::VersionReg));

  nada(2600, 120);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(400); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) Serial.println("IP: " + WiFi.localIP().toString());
  else Serial.println("WiFi GAGAL - alat tetap jalan tanpa internet");

  if (WiFi.status() == WL_CONNECTED && sbLogin()) {
    delay(300);
    sbAmbilPemilik();
    sbAmbilStatus();
    sbAmbilKartu();
  }

  pirTerakhir = digitalRead(PIN_PIR);
  tSiapPir = millis() + JEDA_PIR_SIAP;
  Serial.printf("PIR awal = %s (GPIO %d), siap dalam %d ms\n",
                pirTerakhir ? "HIGH" : "low", PIN_PIR, JEDA_PIR_SIAP);
}

void loop() {
  unsigned long now = millis();

  static unsigned long tLapor = 0;
  if (now - tLapor > 5000) {
    tLapor = now;
    Serial.printf("PIR = %s\n", digitalRead(PIN_PIR) ? "HIGH" : "low");
  }

  if (now - tRfid > 3000) {
    tRfid = now;
    byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    byte tx  = rfid.PCD_ReadRegister(MFRC522::TxControlReg);
    if (ver == 0x00 || ver == 0xFF || !(tx & 0x03)) {
      Serial.printf("RC522 tidur (ver=0x%02X tx=0x%02X) - dibangunkan\n", ver, tx);
      rfid.PCD_Init();
      delay(20);
      rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!token.length() && now - tLogin > 30000) {
      tLogin = now;
      if (sbLogin()) { scanCepat(); sbAmbilPemilik(); scanCepat();
                       sbAmbilStatus(); scanCepat(); sbAmbilKartu(); }
    }
    if (now - tDetak > 30000)            { tDetak = now; sbDetak();       scanCepat(); }
    if (now - tState > 10000)            { tState = now; sbAmbilStatus(); scanCepat(); }
    if (now - tKartu > JEDA_SEGAR_KARTU) { tKartu = now; sbAmbilKartu();  scanCepat(); }
    if (now - tLogin > 2700000)          { sbLogin();                     scanCepat(); }
  }

  if (!sedangAktif() && alarmAktif) {
    Serial.println(">>> ALARM DIMATIKAN dari aplikasi");
    matikanAlarm();
  }

  for (int i = 0; i < 30; i++) {
    tanganiPintu();
    tanganiPir();
    tanganiKartu();
    if (alarmAktif) potonganSirene();
    else delay(7);
  }
}