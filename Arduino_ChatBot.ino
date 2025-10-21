/*
  Arduino Bluetooth Chatbot + MCUFRIEND TFT
  ------------------------------------------
  Features:
  - Clean organized startup screen (no rectangle)
  - White background, black text
  - Black header with white "Bluetooth Chatbot"
  - Startup message centered and neat
  - Simple chat responses (hi, hello, how are you, name, help)
  - Calculator: "calc <expr>" supports +, -, *, /, ^, parentheses, pi, e
*/

#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <math.h>
#if !(defined(ARDUINO_AVR_MEGA2560) || defined(ARDUINO_AVR_MEGA1280))
  #include <SoftwareSerial.h>   // For Uno/Nano
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------- Bluetooth serial ----------
#if defined(ARDUINO_AVR_MEGA2560) || defined(ARDUINO_AVR_MEGA1280)
  #define HAVE_HW_SERIAL1
  #define BT_SERIAL Serial1
#else
  SoftwareSerial BT(10, 11);    // RX=D10, TX=D11
  #define BT_SERIAL BT
#endif

// ---------- TFT ----------
MCUFRIEND_kbv tft;
uint16_t W, H;

// ---------- Colors ----------
#define BLACK   0x0000
#define WHITE   0xFFFF

// ---------- Buffers ----------
char rxBuf[200];
int  rxIdx = 0;
bool haveNew = false;
unsigned long lastByteMs = 0;
const unsigned long MSG_IDLE_TIMEOUT = 400;

// ---------- Labels ----------
const char* LABEL_YOU = "YOU";
const char* LABEL_BOT = "Arduino Bot";

// ---------- Theme ----------
const uint16_t bgColor     = WHITE;
const uint16_t headerColor = BLACK;
const uint16_t headerText  = WHITE;
const uint16_t textColor   = BLACK;

char replyBuf[220];

// ---------- Helpers ----------
static inline void trimInPlace(char* s){
  int st=0; while (s[st]==' '||s[st]=='\t'||s[st]=='\r'||s[st]=='\n') st++;
  if (st) memmove(s, s+st, strlen(s+st)+1);
  int e=strlen(s)-1; while (e>=0 && (s[e]==' '||s[e]=='\t'||s[e]=='\r'||s[e]=='\n')) s[e--]='\0';
}
static inline void toLowerInPlace(char* s){ for (; *s; ++s) if (*s>='A'&&*s<='Z') *s += 32; }
static inline void sanitizeExpr(char* s){
  char* r=s; char* w=s;
  while (*r){
    char c=*r++;
    if ((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||
        c=='+'||c=='-'||c=='*'||c=='/'||c=='^'||c=='('||c==')'||c=='.'||c==' '){
      *w++=c;
    }
  }
  *w='\0';
  trimInPlace(s);
}
static inline const char* istrstr(const char* hay, const char* needle){
  if(!*needle) return hay;
  for (const char* p = hay; *p; ++p){
    const char* h=p; const char* n=needle;
    while (*h && *n){
      char ch = (*h>='A'&&*h<='Z') ? *h+32 : *h;
      char cn = (*n>='A'&&*n<='Z') ? *n+32 : *n;
      if (ch!=cn) break;
      ++h; ++n;
    }
    if (!*n) return p;
  }
  return NULL;
}

// ---------- Drawing ----------
static inline void wrappedPrint(int x, int y, const char* msg, int textSize, int maxWidthPx, int lineStepPx) {
  tft.setTextColor(textColor); tft.setTextSize(textSize); tft.setTextWrap(false);
  int cw = 6 * textSize; int maxChars = (maxWidthPx / cw); if (maxChars < 1) maxChars = 1;
  int len = strlen(msg), start = 0;
  while (start < len && y < (int)H - 4) {
    int take = (len - start < maxChars) ? (len - start) : maxChars;
    int lastSpace = -1; for (int i=0;i<take;i++) if (msg[start+i]==' ') lastSpace = i;
    if (take != len - start && lastSpace >= maxChars/2) take = lastSpace + 1;
    tft.setCursor(x, y);
    for (int i=0;i<take;i++) tft.print(msg[start + i]);
    start += take; while (msg[start]==' ') start++;
    y += lineStepPx;
  }
}
static inline void drawExchange(const char* userMsg, const char* botMsg) {
  tft.fillScreen(bgColor);
  // Header
  tft.fillRect(0, 0, W, 30, headerColor);
  tft.setTextColor(headerText); tft.setTextSize(2); tft.setCursor(10, 8);
  tft.print("Bluetooth Chatbot");

  const int leftPad=10, topStart=40, labelStep=24, lineStep=24;
  const int maxWidth=W-(leftPad*2);
  int y=topStart;

  tft.setTextColor(textColor); tft.setTextSize(2);
  tft.setCursor(leftPad, y); tft.print(LABEL_YOU); tft.print(":"); y += labelStep;
  wrappedPrint(leftPad, y, userMsg, 2, maxWidth, lineStep);

  int cw = 6 * 2; int cols = (maxWidth / cw); if (cols < 1) cols = 1;
  int rowsUser = (strlen(userMsg) + cols - 1) / cols; if (rowsUser < 1) rowsUser = 1;
  y += rowsUser * lineStep + 16;

  tft.setCursor(leftPad, y); tft.print(LABEL_BOT); tft.print(":"); y += labelStep;
  wrappedPrint(leftPad, y, botMsg, 2, maxWidth, lineStep);
}

// ---------- Expression Parser ----------
struct Parser {
  const char* s; int pos; bool ok;
  Parser(const char* src): s(src), pos(0), ok(true) {}
  void skip(){ while (s[pos]==' '||s[pos]=='\t') pos++; }
  bool match(char c){ skip(); if (s[pos]==c){ pos++; return true;} return false; }
  bool parseNumber(double& out){
    skip(); int start=pos;
    if (s[pos]=='+'||s[pos]=='-') pos++;
    bool digits=false;
    while (s[pos]>='0'&&s[pos]<='9'){ digits=true; pos++; }
    if (s[pos]=='.'){ pos++; while (s[pos]>='0'&&s[pos]<='9'){ digits=true; pos++; } }
    if (!digits){ pos=start; return false; }
    if (s[pos]=='e'||s[pos]=='E'){
      int p=pos+1; if (s[p]=='+'||s[p]=='-') p++;
      if (s[p]>='0'&&s[p]<='9'){ pos=p; while (s[pos]>='0'&&s[pos]<='9') pos++; }
    }
    char buf[40]; int n=(pos-start < (int)sizeof(buf)-1)?(pos-start):(int)sizeof(buf)-1;
    strncpy(buf, s+start, n); buf[n]='\0'; out=atof(buf); return true;
  }
  bool parseIdent(char* id, int N){
    skip(); int start=pos;
    if (!((s[pos]>='a'&&s[pos]<='z')||(s[pos]>='A'&&s[pos]<='Z')||s[pos]=='_')) return false;
    pos++;
    while ((s[pos]>='a'&&s[pos]<='z')||(s[pos]>='A'&&s[pos]<='Z')||(s[pos]>='0'&&s[pos]<='9')||s[pos]=='_') pos++;
    int n=(pos-start < N-1)?(pos-start):(N-1); strncpy(id, s+start, n); id[n]='\0';
    return true;
  }
  double parseExpression(){
    double v=parseTerm();
    while (true){
      skip();
      if (s[pos]=='+'){ pos++; v += parseTerm(); }
      else if (s[pos]=='-'){ pos++; v -= parseTerm(); }
      else break;
    }
    return v;
  }
  double parseTerm(){
    double v=parsePower();
    while (true){
      skip();
      if (s[pos]=='*'){ pos++; v *= parsePower(); }
      else if (s[pos]=='/'){ pos++; double d=parsePower(); if (d==0){ ok=false; return NAN; } v /= d; }
      else break;
    }
    return v;
  }
  double parsePower(){
    double base = parseFactor(); skip();
    if (s[pos]=='^'){ pos++; double expn = parsePower(); base = pow(base, expn); }
    return base;
  }
  double parseFactor(){
    skip();
    if (match('+')) return parseFactor();
    if (match('-')) return -parseFactor();
    if (match('(')){
      double v=parseExpression(); if (!match(')')) ok=false; return v;
    }
    double num;
    if (parseNumber(num)) return num;
    char id[8];
    if (parseIdent(id, sizeof(id))){
      for (char* p=id; *p; ++p) if (*p>='A'&&*p<='Z') *p+=32;
      if (!strcmp(id,"pi")) return M_PI;
      if (!strcmp(id,"e"))  return M_E;
      ok=false; return NAN;
    }
    ok=false; return NAN;
  }
  double eval(){ double v=parseExpression(); skip(); if (s[pos]!='\0') ok=false; return v; }
};

static bool evalSimple(const char* expr, double& out){
  Parser p(expr);
  double v = p.eval();
  if (!p.ok || isnan(v)) return false;
  out = v; return true;
}

// ---------- Bot Reply ----------
static void makeReply(const char* user, char* out, size_t n){
  char msg[200]; strncpy(msg, user, sizeof(msg)-1); msg[sizeof(msg)-1]='\0';
  char low[200]; strncpy(low, msg, sizeof(low)-1); low[sizeof(low)-1]='\0';
  trimInPlace(low); toLowerInPlace(low);

  if (istrstr(low,"hello") || istrstr(low,"hi")){
    snprintf(out,n,"Hi! I can chat and do math. Try: calc 2+2");
    return;
  }
  if (istrstr(low,"how are you")){
    snprintf(out,n,"I'm great, thanks for asking!");
    return;
  }
  if (istrstr(low,"name")){
    snprintf(out,n,"I'm %s.", LABEL_BOT);
    return;
  }
  if (!strcmp(low,"help")){
    snprintf(out,n,"Commands: hi, hello, name, how are you, calc <expr>");
    return;
  }

  if (!strncmp(low,"calc ",5)){
    char expr[180]; strncpy(expr, user+5, sizeof(expr)-1); expr[sizeof(expr)-1]='\0';
    trimInPlace(expr); sanitizeExpr(expr);
    if (expr[0]=='\0'){ snprintf(out,n,"Try: calc (2+3)*4"); return; }
    double v;
    if (!evalSimple(expr, v)){ snprintf(out,n,"Can't parse. Try: calc 2+2"); return; }
    char tmp[40]; dtostrf(v, 0, 6, tmp);
    snprintf(out,n,"%s", tmp);
    return;
  }

  snprintf(out,n,"I can chat or do math. Try 'help'.");
}

// ---------- Welcome Screen ----------
void drawWelcomeScreen() {
  tft.fillScreen(WHITE);

  // Header
  tft.fillRect(0, 0, W, 30, BLACK);
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("Bluetooth Chatbot");

  // Body (no rectangle)
  tft.setTextColor(BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 60);
  tft.print("Arduino Bot:");

  wrappedPrint(10, 88, "Hello! Send me a message via Bluetooth.", 2, W - 20, 24);
  wrappedPrint(10, 136, "Type 'help' to see commands.", 2, W - 20, 24);
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);
#if defined(HAVE_HW_SERIAL1)
  Serial.println(F("Using Serial1 (Mega)"));
#else
  Serial.println(F("Using SoftwareSerial on D10/D11 (Uno/Nano)"));
#endif
  BT_SERIAL.begin(9600);

  uint16_t id=tft.readID(); if (id==0xD3D3) id=0x9486;
  tft.begin(id);
  tft.setRotation(1);
  W=tft.width(); H=tft.height();

  drawWelcomeScreen();

  BT_SERIAL.println(F("Arduino Bot: Hello! Send me a message via Bluetooth."));
  BT_SERIAL.println(F("Type 'help' to see commands."));
}

// finalize buffered message if no newline arrives
static inline void finalizeMessageIfAny(){ if (rxIdx>0){ rxBuf[rxIdx]='\0'; haveNew=true; rxIdx=0; }}

// ---------- Loop ----------
void loop(){
  while (BT_SERIAL.available()){
    char c = BT_SERIAL.read();
    lastByteMs = millis();
    if (c=='\r' || c=='\n'){
      if (rxIdx>0){ rxBuf[rxIdx]='\0'; haveNew=true; rxIdx=0; }
    }else if (rxIdx < (int)sizeof(rxBuf)-1){
      rxBuf[rxIdx++] = c;
    }
  }
  if (rxIdx>0 && (millis()-lastByteMs)>MSG_IDLE_TIMEOUT) finalizeMessageIfAny();

  if (haveNew){
    haveNew=false;
    trimInPlace(rxBuf);
    if (rxBuf[0]){
      makeReply(rxBuf, replyBuf, sizeof(replyBuf));
      drawExchange(rxBuf, replyBuf);
      BT_SERIAL.print(LABEL_YOU); BT_SERIAL.print(F(": ")); BT_SERIAL.println(rxBuf);
      BT_SERIAL.print(LABEL_BOT); BT_SERIAL.print(F(": ")); BT_SERIAL.println(replyBuf);
    }
  }
}
