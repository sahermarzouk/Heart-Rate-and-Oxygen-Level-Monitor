#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>

const char* ssid = "Marzouk";       
const char* password = "12345678"; 

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
LiquidCrystal_PCF8574 lcd(0x27);

int beatAvg = 0;
int spo2 = 0;
long lastBeat = 0;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Bousla Health</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; background: #121212; color: white; margin: 0; padding: 10px; }
        h2 { color: #00e676; margin-bottom: 5px; }
        .card { background: #1e1e1e; padding: 15px; border-radius: 12px; margin: 10px auto; max-width: 400px; box-shadow: 0 4px 8px rgba(0,0,0,0.3); }
        .heart { font-size: 50px; color: #ff3b30; display: inline-block; transition: transform 0.1s cubic-bezier(0.175, 0.885, 0.32, 1.275); }
        .heart.beating { transform: scale(1.4); }
        .value { font-size: 35px; font-weight: bold; color: #00e676; }
        .label { font-size: 14px; color: #aaaaaa; }
        canvas { max-width: 100%; margin-top: 10px; }
    </style>
</head>
<body>
    <h2>Bousla Health</h2>
    <div class="card">
        <div id="heart-icon" class="heart">&#10084;</div>
        <div class="value"><span id="bpm">0</span> <span class="label">BPM</span></div>
    </div>
    <div class="card">
        <div class="value"><span id="spo2">0</span> <span class="label">% SpO2</span></div>
    </div>
    <div class="card">
        <canvas id="signalChart"></canvas>
    </div>
    <script>
        let gateway = `ws://${window.location.hostname}/ws`;
        let websocket = new WebSocket(gateway);
        let chart;
        
        window.onload = () => {
            const ctx = document.getElementById('signalChart').getContext('2d');
            chart = new Chart(ctx, {
                type: 'line',
                data: { labels: Array(50).fill(''), datasets: [{ label: 'PPG Signal', data: [], borderColor: '#00e676', borderWidth: 2, tension: 0.3, pointRadius: 0 }] },
                options: { animation: false, scales: { x: { display: false }, y: { display: false } }, plugins: { legend: { display: false } } }
            });
        };

        websocket.onmessage = (event) => {
            let data = JSON.parse(event.data);
            document.getElementById('bpm').innerHTML = data.bpm;
            document.getElementById('spo2').innerHTML = data.spo2;
            
            chart.data.datasets[0].data.push(data.ir);
            if(chart.data.datasets[0].data.length > 50) chart.data.datasets[0].data.shift();
            chart.update();

            if (data.beat) {
                let heart = document.getElementById('heart-icon');
                heart.classList.add('beating');
                setTimeout(() => { heart.classList.remove('beating'); }, 150);
            }
        };
    </script>
</body>
</html>
)rawliteral";

void setupSensor() {
  Wire.beginTransmission(0x57); Wire.write(0x09); Wire.write(0x40); Wire.endTransmission(); delay(100); 
  Wire.beginTransmission(0x57); Wire.write(0x08); Wire.write(0x27); Wire.endTransmission(); 
  Wire.beginTransmission(0x57); Wire.write(0x09); Wire.write(0x03); Wire.endTransmission(); 
  Wire.beginTransmission(0x57); Wire.write(0x0A); Wire.write(0x27); Wire.endTransmission(); 
  Wire.beginTransmission(0x57); Wire.write(0x0C); Wire.write(0x24); Wire.endTransmission(); 
  Wire.beginTransmission(0x57); Wire.write(0x0D); Wire.write(0x24); Wire.endTransmission(); 
}

void readSensor(long &red, long &ir) {
  red = 0; ir = 0;
  Wire.beginTransmission(0x57); Wire.write(0x07); Wire.endTransmission(false);
  Wire.requestFrom(0x57, 6);
  if (Wire.available() == 6) {
    red = ((long)Wire.read() << 16 | (long)Wire.read() << 8 | Wire.read()) & 0x03FFFF;
    ir = ((long)Wire.read() << 16 | (long)Wire.read() << 8 | Wire.read()) & 0x03FFFF;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  lcd.begin(16, 2);
  lcd.setBacklight(255);
  lcd.clear();
  lcd.print("Starting AP...");

  setupSensor(); 

  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP(); 
  
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("WiFi: Marzouk");
  lcd.setCursor(0, 1); lcd.print(IP);
  delay(4000); 
  
  lcd.clear();
  lcd.print("Place Finger");

  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });
  server.begin();
}

void loop() {
  long redValue, irValue;
  readSensor(redValue, irValue);

  static long lastRawIR = 0;
  bool isNewData = (irValue != lastRawIR && irValue != 0);
  lastRawIR = irValue;

  static unsigned long sampleStartTime = millis();
  static long irMin = 999999, irMax = 0;
  static long redMin = 999999, redMax = 0;
  static float irAvg = 0; 
  static bool aboveAvg = false;
  static bool realTimeBeat = false;

  if (isNewData) {
    if (irValue < 50000) {
      beatAvg = 0; spo2 = 0;
      irMin = 999999; irMax = 0; redMin = 999999; redMax = 0;
    } else {
      irAvg = (irAvg * 0.95) + (irValue * 0.05); 
      
      if (irValue > irAvg + 20) { 
        if (!aboveAvg) {
          aboveAvg = true;
          realTimeBeat = true;

          if (millis() - lastBeat > 350) { 
            long delta = millis() - lastBeat;
            lastBeat = millis();
            float currentBpm = 60000.0 / delta;
            if (currentBpm > 40 && currentBpm < 160) {
              beatAvg = (beatAvg == 0) ? currentBpm : (beatAvg * 0.7) + (currentBpm * 0.3);
            }
          }
        }
      } else if (irValue < irAvg) {
        aboveAvg = false; 
      }

      if (irValue < irMin) irMin = irValue;
      if (irValue > irMax) irMax = irValue;
      if (redValue < redMin) redMin = redValue;
      if (redValue > redMax) redMax = redValue;

      if (millis() - sampleStartTime > 1000) {
        long irAC = irMax - irMin;
        long redAC = redMax - redMin;

        if (irAC < 30) {
          beatAvg = 0; spo2 = 0;
        } else {
          float irDC = (irMax + irMin) / 2.0;
          float redDC = (redMax + redMin) / 2.0;
          float ratio = (redAC / redDC) / (irAC / irDC);
          
          int calcSpo2 = 110 - (25.0 * ratio); 
          if (calcSpo2 > 100) calcSpo2 = 100;
          if (calcSpo2 < 70) calcSpo2 = 70; 
          
          spo2 = (spo2 == 0) ? calcSpo2 : (spo2 * 0.8) + (calcSpo2 * 0.2); 
        }
        irMin = 999999; irMax = 0; redMin = 999999; redMax = 0;
        sampleStartTime = millis();
      }
    }
  }

  static unsigned long lastWsUpdate = 0;
  if (millis() - lastWsUpdate > 50) {
    String json = "{\"bpm\":" + String(beatAvg) + ",\"spo2\":" + String(spo2) + ",\"ir\":" + String(irValue) + ",\"beat\":" + (realTimeBeat ? "true" : "false") + "}";
    ws.textAll(json);
    lastWsUpdate = millis();
    realTimeBeat = false;
  }
  
  static unsigned long lastLcdUpdate = 0;
  if (millis() - lastLcdUpdate > 500) {
    if (irValue < 50000 || (irMax - irMin) < 30) {
      lcd.setCursor(0, 0); lcd.print("No Pulse!       ");
      lcd.setCursor(0, 1); lcd.print("                ");
    } else {
      lcd.setCursor(0, 0); lcd.print("BPM: "); lcd.print(beatAvg); lcd.print("    ");
      lcd.setCursor(0, 1); lcd.print("SpO2: "); lcd.print(spo2); lcd.print("%   ");
    }
    lastLcdUpdate = millis();
  }
  
  ws.cleanupClients();
  delay(10);
}