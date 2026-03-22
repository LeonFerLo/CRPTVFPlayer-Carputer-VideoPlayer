#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>

// ========================================================
// CONFIG
// ========================================================
static const char* VIDEO_EXT  = ".crptvf";
static const char* APP_DIR    = "/CRPTVFPlayer";
static const char* PREFS_FILE = "/CRPTVFPlayer/preferences.config";
static const char* DEFAULT_BROWSER_PATH = "/videos";

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

static constexpr uint16_t COLOR_BG  = 0x0000;
static constexpr uint16_t COLOR_FG  = 0xFFFF;
static constexpr uint16_t COLOR_ERR = 0xF800;
static constexpr uint16_t COLOR_DIM = 0x7BEF;
static constexpr uint16_t COLOR_SEL = 0x07E0;
static constexpr uint16_t COLOR_BAR = 0x001F;
static constexpr uint16_t COLOR_VOL = 0x07E0;
static constexpr uint16_t COLOR_BRI = 0xFFE0;

static constexpr int BLOCK_H = 8;
static constexpr int VISIBLE_ROWS = 7;

// Pines SD Cardputer / Cardputer ADV
static constexpr int SD_SPI_SCK_PIN  = 40;
static constexpr int SD_SPI_MISO_PIN = 39;
static constexpr int SD_SPI_MOSI_PIN = 14;
static constexpr int SD_SPI_CS_PIN   = 12;

// ========================================================
// GLOBAL CONFIG
// ========================================================
struct AppConfig {
  String browserStartPath = DEFAULT_BROWSER_PATH;

  // 1 = frames/frames
  // 2 = mm:ss/mm:ss
  // 3 = ambos
  uint8_t timeDisplayMode = 3;

  // 1 = fps del video
  // 2 = fps reales
  // 3 = ambos
  uint8_t fpsDisplayMode = 3;

  bool showBars = true;

  int volume = 180;       // 0..255
  int brightness = 128;   // 0..255
};

AppConfig g_cfg;

// ========================================================
// TYPES
// ========================================================
struct BrowserItem {
  String name;
  String path;
  bool isDir;
};

enum Action {
  ACT_NONE,
  ACT_UP,
  ACT_DOWN,
  ACT_LEFT,
  ACT_RIGHT,
  ACT_ENTER,
  ACT_BACK,
  ACT_VOL_DOWN,
  ACT_VOL_UP,
  ACT_BRI_DOWN,
  ACT_BRI_UP,
  ACT_SAVE_PATH
};

enum StartMenuResult {
  MENU_PLAY = 0,
  MENU_CONFIG = 1
};

#pragma pack(push, 1)
struct CRPTVFHeader {
  char magic[8];              // "CRPTVF1\0"
  uint16_t width;
  uint16_t height;
  uint16_t fps;
  uint32_t frame_count;
  uint16_t palette_size;
  uint8_t bits_per_pixel;
  uint8_t flags;
  uint8_t audio_codec;
  uint8_t audio_channels;
  uint32_t audio_sample_rate;
  uint32_t video_offset;
  uint32_t video_size;
  uint32_t audio_offset;
  uint32_t audio_size;
  uint8_t reserved[20];
};
#pragma pack(pop)

static constexpr size_t HEADER_SIZE = 64;
static constexpr size_t MAX_PALETTE_SIZE = 256;

// ========================================================
// HELPERS
// ========================================================
bool isValidPaletteSize(uint16_t n) {
  switch (n) {
    case 8:
    case 16:
    case 64:
    case 128:
    case 256:
      return true;
    default:
      return false;
  }
}

void clearScreen(uint16_t color = COLOR_BG) {
  M5Cardputer.Display.fillScreen(color);
}

void fillRectSafe(int x, int y, int w, int h, uint16_t color) {
  M5Cardputer.Display.fillRect(x, y, w, h, color);
}

void drawText(const String& txt, int x, int y, uint16_t color = COLOR_FG, uint16_t bg = COLOR_BG) {
  M5Cardputer.Display.setTextColor(color, bg);
  M5Cardputer.Display.setCursor(x, y);
  M5Cardputer.Display.print(txt);
}

void drawCenteredText(const String& txt, int y, uint16_t color = COLOR_FG, uint16_t bg = COLOR_BG) {
  int x = max(0, (SCREEN_W - (int)txt.length() * 6) / 2);
  drawText(txt, x, y, color, bg);
}

void showMessage(const String& title, const String& msg = "", uint16_t color = COLOR_FG, int waitMs = 1200) {
  clearScreen(COLOR_BG);
  drawCenteredText(title, 42, color);
  if (msg.length()) {
    drawCenteredText(msg.substring(0, 36), 62, COLOR_DIM);
  }
  if (waitMs > 0) delay(waitMs);
}

String basenameOf(const String& path) {
  int p = path.lastIndexOf('/');
  if (p < 0) return path;
  return path.substring(p + 1);
}

String normalizePath(const String& p) {
  String out = p;
  if (out.length() == 0) return "/";
  if (!out.startsWith("/")) out = "/" + out;
  while (out.endsWith("/") && out.length() > 1) {
    out.remove(out.length() - 1);
  }
  return out;
}

String parentDir(const String& path) {
  String p = normalizePath(path);
  if (p == "/") return "/";
  int slash = p.lastIndexOf('/');
  if (slash <= 0) return "/";
  return p.substring(0, slash);
}

bool endsWithIgnoreCase(const String& s, const String& suffix) {
  if (s.length() < suffix.length()) return false;
  String a = s;
  String b = suffix;
  a.toLowerCase();
  b.toLowerCase();
  return a.endsWith(b);
}

String trimStr(const String& s) {
  String out = s;
  out.trim();
  return out;
}

bool toBool(const String& s) {
  String v = s;
  v.toLowerCase();
  return (v == "1" || v == "true" || v == "yes" || v == "on");
}

String formatTimeMMSS(uint32_t totalSeconds) {
  uint32_t mm = totalSeconds / 60;
  uint32_t ss = totalSeconds % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}

void drawMiniBar(int x, int y, int w, int h, int value, uint16_t borderColor, uint16_t fillColor) {
  if (value < 0) value = 0;
  if (value > 255) value = 255;

  M5Cardputer.Display.drawRect(x, y, w, h, borderColor);

  int innerW = w - 2;
  int innerH = h - 2;
  if (innerW < 1 || innerH < 1) return;

  M5Cardputer.Display.fillRect(x + 1, y + 1, innerW, innerH, COLOR_BG);

  int fillW = (value * innerW) / 255;
  if (fillW > 0) {
    M5Cardputer.Display.fillRect(x + 1, y + 1, fillW, innerH, fillColor);
  }
}

void applyVolume() {
  if (g_cfg.volume < 0) g_cfg.volume = 0;
  if (g_cfg.volume > 255) g_cfg.volume = 255;
  M5Cardputer.Speaker.setVolume(g_cfg.volume);
}

void applyBrightness() {
  if (g_cfg.brightness < 0) g_cfg.brightness = 0;
  if (g_cfg.brightness > 255) g_cfg.brightness = 255;
  M5Cardputer.Display.setBrightness(g_cfg.brightness);
}

void ensureAppDir() {
  if (!SD.exists(APP_DIR)) {
    SD.mkdir(APP_DIR);
  }
}

void applyConfig() {
  g_cfg.browserStartPath = normalizePath(g_cfg.browserStartPath);
  if (g_cfg.browserStartPath.length() == 0) g_cfg.browserStartPath = DEFAULT_BROWSER_PATH;
  if (g_cfg.timeDisplayMode < 1 || g_cfg.timeDisplayMode > 3) g_cfg.timeDisplayMode = 3;
  if (g_cfg.fpsDisplayMode < 1 || g_cfg.fpsDisplayMode > 3) g_cfg.fpsDisplayMode = 3;
  applyVolume();
  applyBrightness();
}

void saveConfig() {
  ensureAppDir();

  if (SD.exists(PREFS_FILE)) {
    SD.remove(PREFS_FILE);
  }

  File f = SD.open(PREFS_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("No se pudo escribir preferences.config");
    return;
  }

  f.println("# CRPTVFPlayer preferences");
  f.println("browser_start_path=" + g_cfg.browserStartPath);
  f.println("time_display_mode=" + String(g_cfg.timeDisplayMode));
  f.println("fps_display_mode=" + String(g_cfg.fpsDisplayMode));
  f.println("show_bars=" + String(g_cfg.showBars ? 1 : 0));
  f.println("volume=" + String(g_cfg.volume));
  f.println("brightness=" + String(g_cfg.brightness));
  f.close();
}

void loadConfig() {
  ensureAppDir();

  if (!SD.exists(PREFS_FILE)) {
    applyConfig();
    saveConfig();
    return;
  }

  File f = SD.open(PREFS_FILE, FILE_READ);
  if (!f) {
    applyConfig();
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq < 0) continue;

    String key = trimStr(line.substring(0, eq));
    String value = trimStr(line.substring(eq + 1));

    if (key == "browser_start_path") {
      g_cfg.browserStartPath = value;
    } else if (key == "time_display_mode") {
      g_cfg.timeDisplayMode = (uint8_t)value.toInt();
    } else if (key == "fps_display_mode") {
      g_cfg.fpsDisplayMode = (uint8_t)value.toInt();
    } else if (key == "show_bars") {
      g_cfg.showBars = toBool(value);
    } else if (key == "volume") {
      g_cfg.volume = value.toInt();
    } else if (key == "brightness") {
      g_cfg.brightness = value.toInt();
    }
  }

  f.close();
  applyConfig();
}

// ========================================================
// SD
// ========================================================
bool initSD() {
  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    Serial.println("SD init failed");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card attached");
    return false;
  }
  return true;
}

// ========================================================
// INPUT
// ========================================================
Action readAction() {
  M5Cardputer.update();

  if (M5Cardputer.BtnA.wasPressed()) {
    return ACT_BACK;
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto status = M5Cardputer.Keyboard.keysState();

    if (status.enter) return ACT_ENTER;

    for (auto c : status.word) {
      char ch = (char)c;
      if (ch >= 'a' && ch <= 'z') ch -= 32;

      switch (ch) {
        case 'W': return ACT_UP;
        case 'S': return ACT_DOWN;
        case 'A': return ACT_LEFT;
        case 'D': return ACT_RIGHT;
        case 'Q': return ACT_BACK;
        case 'E': return ACT_ENTER;
        case ' ': return ACT_ENTER;
        case '-': return ACT_VOL_DOWN;
        case '=': return ACT_VOL_UP;
        case '[': return ACT_BRI_DOWN;
        case ']': return ACT_BRI_UP;
        case 'F': return ACT_SAVE_PATH;
        default: break;
      }
    }
  }

  return ACT_NONE;
}

// ========================================================
// FILE BROWSER
// ========================================================
std::vector<BrowserItem> listDirItems(const String& path, bool includeFiles) {
  std::vector<BrowserItem> items;
  String p = normalizePath(path);

  if (p != "/") {
    items.push_back({"[..]", "__PARENT__", true});
  }

  File root = SD.open(p);
  if (!root || !root.isDirectory()) {
    Serial.printf("Cannot open dir: %s\n", p.c_str());
    return items;
  }

  std::vector<BrowserItem> dirs;
  std::vector<BrowserItem> files;

  File entry = root.openNextFile();
  while (entry) {
    String name = String(entry.name());
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);

    String fullPath = p;
    if (!fullPath.endsWith("/")) fullPath += "/";
    fullPath += name;
    fullPath = normalizePath(fullPath);

    if (entry.isDirectory()) {
      dirs.push_back({name, fullPath, true});
    } else if (includeFiles && endsWithIgnoreCase(name, VIDEO_EXT)) {
      files.push_back({name, fullPath, false});
    }

    entry.close();
    entry = root.openNextFile();
  }
  root.close();

  auto sorter = [](const BrowserItem& a, const BrowserItem& b) {
    String aa = a.name; aa.toLowerCase();
    String bb = b.name; bb.toLowerCase();
    return aa < bb;
  };

  std::sort(dirs.begin(), dirs.end(), sorter);
  std::sort(files.begin(), files.end(), sorter);

  items.insert(items.end(), dirs.begin(), dirs.end());
  items.insert(items.end(), files.begin(), files.end());
  return items;
}

void drawBrowser(const std::vector<BrowserItem>& items, int selected, int offset, const String& currentDir) {
  clearScreen(COLOR_BG);

  fillRectSafe(0, 0, SCREEN_W, 14, COLOR_BAR);
  drawText("CRPTVF Player", 4, 3, COLOR_FG, COLOR_BAR);
  drawText(currentDir.substring(0, 34), 4, 18, COLOR_DIM, COLOR_BG);

  const int startY = 34;
  const int rowH = 13;

  if (items.empty()) {
    drawText("Carpeta vacia", 6, 54, COLOR_ERR, COLOR_BG);
    drawText("Q o BtnA salir", 6, 120, COLOR_DIM, COLOR_BG);
    return;
  }

  for (int i = 0; i < VISIBLE_ROWS; ++i) {
    int idx = offset + i;
    if (idx >= (int)items.size()) break;

    int y = startY + i * rowH;
    const auto& item = items[idx];

    String label;
    if (item.path == "__PARENT__") {
      label = "[..]";
    } else if (item.isDir) {
      label = "[DIR] " + item.name;
    } else {
      label = item.name;
    }
    label = label.substring(0, 34);

    if (idx == selected) {
      fillRectSafe(2, y - 1, SCREEN_W - 4, rowH, COLOR_SEL);
      drawText(label, 6, y, COLOR_BG, COLOR_SEL);
    } else {
      drawText(label, 6, y, COLOR_FG, COLOR_BG);
    }
  }

  drawText("W/S mover  E/D abrir", 4, 108, COLOR_DIM, COLOR_BG);
  drawText("A/Q o BtnA volver", 4, 121, COLOR_DIM, COLOR_BG);
}

String browseForFolderAndSave(const String& initialPath) {
  String currentDir = normalizePath(initialPath);
  if (!SD.exists(currentDir)) currentDir = "/";

  int selected = 0;
  int offset = 0;
  bool dirty = true;

  while (true) {
    std::vector<BrowserItem> items = listDirItems(currentDir, false);

    if (selected < 0) selected = 0;
    if (!items.empty() && selected >= (int)items.size()) selected = (int)items.size() - 1;
    if (selected < offset) offset = selected;
    if (selected >= offset + VISIBLE_ROWS) offset = selected - VISIBLE_ROWS + 1;

    if (dirty) {
      clearScreen(COLOR_BG);
      fillRectSafe(0, 0, SCREEN_W, 14, COLOR_BAR);
      drawText("Elegir carpeta", 4, 3, COLOR_FG, COLOR_BAR);
      drawText(currentDir.substring(0, 34), 4, 18, COLOR_DIM, COLOR_BG);

      const int startY = 34;
      const int rowH = 13;

      if (items.empty()) {
        drawText("Sin carpetas", 6, 54, COLOR_ERR, COLOR_BG);
      } else {
        for (int i = 0; i < VISIBLE_ROWS; ++i) {
          int idx = offset + i;
          if (idx >= (int)items.size()) break;

          int y = startY + i * rowH;
          String label = items[idx].path == "__PARENT__" ? "[..]" : "[DIR] " + items[idx].name;
          label = label.substring(0, 34);

          if (idx == selected) {
            fillRectSafe(2, y - 1, SCREEN_W - 4, rowH, COLOR_SEL);
            drawText(label, 6, y, COLOR_BG, COLOR_SEL);
          } else {
            drawText(label, 6, y, COLOR_FG, COLOR_BG);
          }
        }
      }

      fillRectSafe(0, SCREEN_H - 12, SCREEN_W, 12, COLOR_BG);
      drawText("E abre | F guarda | Q vuelve", 2, SCREEN_H - 10, COLOR_DIM, COLOR_BG);

      dirty = false;
    }

    Action action = readAction();

    if (action == ACT_UP) {
      if (selected > 0) {
        selected--;
        dirty = true;
      }
    } else if (action == ACT_DOWN) {
      if (selected < (int)items.size() - 1) {
        selected++;
        dirty = true;
      }
    } else if (action == ACT_ENTER || action == ACT_RIGHT) {
      if (!items.empty()) {
        const BrowserItem& item = items[selected];
        if (item.path == "__PARENT__") {
          currentDir = parentDir(currentDir);
        } else if (item.isDir) {
          currentDir = item.path;
        }
        selected = 0;
        offset = 0;
        dirty = true;
      }
    } else if (action == ACT_SAVE_PATH) {
      return currentDir;
    } else if (action == ACT_BACK || action == ACT_LEFT) {
      return "";
    }

    delay(20);
  }
}

// ========================================================
// CRPTVF FILE
// ========================================================
class CRPTVFFile {
public:
  File f;
  String path;

  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t fps = 0;
  uint32_t frameCount = 0;
  uint16_t paletteSize = 0;
  uint8_t bitsPerPixel = 0;
  uint8_t flags = 0;

  uint8_t audioCodec = 0;
  uint8_t audioChannels = 0;
  uint32_t audioSampleRate = 0;
  uint32_t audioOffset = 0;
  uint32_t audioSize = 0;

  uint32_t frameDataOffset = 0;
  uint32_t videoSize = 0;
  uint32_t frameSize = 0;

  uint16_t palette565[MAX_PALETTE_SIZE];

  std::vector<uint8_t> blockIdxBuf;
  std::vector<uint16_t> blockRgbBuf;

  bool openFile(const String& filePath, String& err) {
    path = filePath;
    f = SD.open(path, FILE_READ);
    if (!f) {
      err = "no se pudo abrir";
      return false;
    }

    if (f.size() < (int)HEADER_SIZE) {
      err = "header incompleto";
      f.close();
      return false;
    }

    CRPTVFHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
      err = "error leyendo header";
      f.close();
      return false;
    }

    if (memcmp(hdr.magic, "CRPTVF1\0", 8) != 0) {
      err = "magic invalido";
      f.close();
      return false;
    }

    width = hdr.width;
    height = hdr.height;
    fps = hdr.fps;
    frameCount = hdr.frame_count;
    paletteSize = hdr.palette_size;
    bitsPerPixel = hdr.bits_per_pixel;
    flags = hdr.flags;
    audioCodec = hdr.audio_codec;
    audioChannels = hdr.audio_channels;
    audioSampleRate = hdr.audio_sample_rate;
    frameDataOffset = hdr.video_offset;
    videoSize = hdr.video_size;
    audioOffset = hdr.audio_offset;
    audioSize = hdr.audio_size;

    if (width == 0 || height == 0) {
      err = "resolucion invalida";
      f.close();
      return false;
    }
    if (fps == 0) {
      err = "fps invalido";
      f.close();
      return false;
    }
    if (frameCount == 0) {
      err = "sin frames";
      f.close();
      return false;
    }
    if (!isValidPaletteSize(paletteSize)) {
      err = "palette_size invalido";
      f.close();
      return false;
    }
    if (bitsPerPixel != 8) {
      err = "bits_per_pixel no soportado";
      f.close();
      return false;
    }

    size_t paletteBytes = (size_t)paletteSize * 2;
    std::vector<uint8_t> pal(paletteBytes);

    if (f.read(pal.data(), paletteBytes) != (int)paletteBytes) {
      err = "paleta incompleta";
      f.close();
      return false;
    }

    // Ojo: el usuario confirmó que esta forma se ve correcta
    for (int i = 0; i < paletteSize; ++i) {
      uint8_t hi = pal[i * 2 + 0];
      uint8_t lo = pal[i * 2 + 1];
      palette565[i] = ((uint16_t)lo << 8) | hi;
    }

    for (int i = paletteSize; i < MAX_PALETTE_SIZE; ++i) {
      palette565[i] = 0;
    }

    frameSize = (uint32_t)width * (uint32_t)height;

    uint32_t expectedVideoSize = frameCount * frameSize;
    if (videoSize < expectedVideoSize) {
      frameCount = videoSize / frameSize;
      if (frameCount == 0) {
        err = "video sin frames validos";
        f.close();
        return false;
      }
    }

    blockIdxBuf.resize(width * BLOCK_H);
    blockRgbBuf.resize(width * BLOCK_H);
    return true;
  }

  void closeFile() {
    if (f) f.close();
  }

  bool seekFrame(uint32_t frameIndex) {
    uint32_t pos = frameDataOffset + frameIndex * frameSize;
    return f.seek(pos);
  }

  bool readBlockIndexes(int rows) {
    size_t need = (size_t)width * rows;
    return f.read(blockIdxBuf.data(), need) == (int)need;
  }

  void convertBlockIndexesToRGB(int rows) {
    for (int row = 0; row < rows; ++row) {
      uint8_t* src = blockIdxBuf.data() + (row * width);
      uint16_t* dst = blockRgbBuf.data() + (row * width);
      for (int x = 0; x < width; ++x) {
        uint8_t idx = src[x];
        dst[x] = (idx < paletteSize) ? palette565[idx] : 0;
      }
    }
  }
};

// ========================================================
// AUDIO
// ========================================================
struct AudioStreamer {
  File f;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t pos = 0;
  uint32_t sampleRate = 16000;
  bool stereo = false;
  bool enabled = false;

  static constexpr int CHANNEL = 0;
  static constexpr size_t CHUNK = 1024;
  static constexpr int NUM_BUFS = 3;

  struct Slot {
    uint8_t data[CHUNK];
    size_t len = 0;
    bool busy = false;
    uint32_t releaseMs = 0;
  };

  Slot slots[NUM_BUFS];
  uint32_t scheduledEndMs = 0;

  bool begin(const String& path,
             uint32_t audioOffset,
             uint32_t audioSize,
             uint32_t sr,
             uint8_t channels,
             uint8_t codec) {
    if (codec != 1 || audioSize == 0) {
      enabled = false;
      return false;
    }

    f = SD.open(path, FILE_READ);
    if (!f) {
      enabled = false;
      return false;
    }

    offset = audioOffset;
    size = audioSize;
    pos = 0;
    sampleRate = sr;
    stereo = (channels > 1);

    if (!f.seek(offset)) {
      f.close();
      enabled = false;
      return false;
    }

    for (int i = 0; i < NUM_BUFS; ++i) {
      slots[i].len = 0;
      slots[i].busy = false;
      slots[i].releaseMs = 0;
    }

    scheduledEndMs = millis();
    enabled = true;
    service(); service(); service();
    return true;
  }

  void stop() {
    if (f) f.close();
    enabled = false;
    pos = 0;
    scheduledEndMs = 0;
    for (int i = 0; i < NUM_BUFS; ++i) {
      slots[i].len = 0;
      slots[i].busy = false;
      slots[i].releaseMs = 0;
    }
    M5Cardputer.Speaker.stop(CHANNEL);
  }

  void reset() {
    if (!enabled) return;
    pos = 0;
    f.seek(offset);
    scheduledEndMs = millis();
    for (int i = 0; i < NUM_BUFS; ++i) {
      slots[i].len = 0;
      slots[i].busy = false;
      slots[i].releaseMs = 0;
    }
    M5Cardputer.Speaker.stop(CHANNEL);
    service(); service(); service();
  }

  void seekToByte(uint32_t newPos) {
    if (!enabled) return;
    if (newPos > size) newPos = size;
    pos = newPos;
    f.seek(offset + pos);
    scheduledEndMs = millis();
    for (int i = 0; i < NUM_BUFS; ++i) {
      slots[i].len = 0;
      slots[i].busy = false;
      slots[i].releaseMs = 0;
    }
    M5Cardputer.Speaker.stop(CHANNEL);
    service(); service();
  }

  void releaseFinishedSlots() {
    uint32_t now = millis();
    for (int i = 0; i < NUM_BUFS; ++i) {
      if (slots[i].busy && (int32_t)(now - slots[i].releaseMs) >= 0) {
        slots[i].busy = false;
      }
    }
  }

  int findFreeSlot() {
    for (int i = 0; i < NUM_BUFS; ++i) {
      if (!slots[i].busy) return i;
    }
    return -1;
  }

  uint32_t chunkDurationMs(size_t lenBytes) const {
    uint32_t bytesPerSampleFrame = stereo ? 2 : 1;  // PCM_U8
    uint32_t sampleFrames = lenBytes / bytesPerSampleFrame;
    if (sampleRate == 0) return 0;
    return (sampleFrames * 1000UL) / sampleRate;
  }

  void queueOne() {
    if (!enabled || pos >= size) return;

    releaseFinishedSlots();

    int idx = findFreeSlot();
    if (idx < 0) return;

    size_t remain = size - pos;
    size_t len = (remain < CHUNK) ? remain : CHUNK;

    int rd = f.read(slots[idx].data, len);
    if (rd <= 0) return;

    slots[idx].len = (size_t)rd;

    bool ok = M5Cardputer.Speaker.playRaw(
      slots[idx].data,
      slots[idx].len,
      sampleRate,
      stereo,
      1,
      CHANNEL,
      false
    );

    if (!ok) {
      f.seek(f.position() - rd);
      return;
    }

    uint32_t now = millis();
    uint32_t dur = chunkDurationMs(slots[idx].len);
    uint32_t startAt = (scheduledEndMs > now) ? scheduledEndMs : now;
    uint32_t endAt = startAt + dur + 2;

    slots[idx].busy = true;
    slots[idx].releaseMs = endAt;
    scheduledEndMs = endAt;
    pos += rd;
  }

  void service() {
    if (!enabled) return;
    releaseFinishedSlots();
    for (int i = 0; i < NUM_BUFS; ++i) {
      queueOne();
    }
  }
};

// ========================================================
// DRAW
// ========================================================
void drawVideoBackground(int dx, int dy, int dw, int dh) {
  clearScreen(COLOR_BG);

  if (dy > 0) fillRectSafe(0, 0, SCREEN_W, dy, COLOR_BG);

  int bottomH = SCREEN_H - (dy + dh);
  if (bottomH > 0) fillRectSafe(0, dy + dh, SCREEN_W, bottomH, COLOR_BG);

  if (dx > 0) fillRectSafe(0, dy, dx, dh, COLOR_BG);

  int rightW = SCREEN_W - (dx + dw);
  if (rightW > 0) fillRectSafe(dx + dw, dy, rightW, dh, COLOR_BG);
}

void drawPlayerOverlay(const String& filename,
                       uint32_t frameIndex,
                       uint32_t frameCount,
                       bool paused,
                       uint16_t fileFps,
                       uint16_t realFps) {
  fillRectSafe(0, 0, SCREEN_W, 22, COLOR_BG);

  String status = paused ? "PAUSE" : "PLAY";

  uint32_t curSec = 0;
  uint32_t totalSec = 0;

  if (fileFps > 0) {
    curSec = frameIndex / fileFps;
    totalSec = frameCount / fileFps;
  }

  String progressStr;
  if (g_cfg.timeDisplayMode == 1) {
    progressStr = String(frameIndex + 1) + "/" + String(frameCount);
  } else if (g_cfg.timeDisplayMode == 2) {
    progressStr = formatTimeMMSS(curSec) + "/" + formatTimeMMSS(totalSec);
  } else {
    progressStr = String(frameIndex + 1) + "/" + String(frameCount) + " " +
                  formatTimeMMSS(curSec) + "/" + formatTimeMMSS(totalSec);
  }

  String fpsStr;
  if (g_cfg.fpsDisplayMode == 1) {
    fpsStr = "VF:" + String(fileFps);
  } else if (g_cfg.fpsDisplayMode == 2) {
    fpsStr = "RF:" + String(realFps);
  } else {
    fpsStr = "VF:" + String(fileFps) + " RF:" + String(realFps);
  }

  drawText((status + " " + progressStr).substring(0, 39), 2, 2, COLOR_FG, COLOR_BG);
  drawText(fpsStr.substring(0, 26), 2, 11, COLOR_DIM, COLOR_BG);

  if (g_cfg.showBars) {
    drawText("V", 168, 2, COLOR_FG, COLOR_BG);
    drawMiniBar(176, 2, 58, 8, g_cfg.volume, COLOR_FG, COLOR_VOL);

    drawText("B", 168, 11, COLOR_FG, COLOR_BG);
    drawMiniBar(176, 11, 58, 8, g_cfg.brightness, COLOR_FG, COLOR_BRI);
  }

  fillRectSafe(0, SCREEN_H - 12, SCREEN_W, 12, COLOR_BG);
  drawText("A/D seek E pausa Q sale", 2, SCREEN_H - 10, COLOR_DIM, COLOR_BG);
}

// ========================================================
// PLAYER
// ========================================================
void playCRPTVF(const String& path) {
  String err;
  CRPTVFFile video;

  if (!video.openFile(path, err)) {
    showMessage("Error abriendo", err, COLOR_ERR, 1800);
    return;
  }

  const int topUI = 22;
  const int bottomUI = 12;
  const int usableH = SCREEN_H - topUI - bottomUI;

  int dw = video.width;
  int dh = video.height;
  int dx = (SCREEN_W - dw) / 2;
  int dy = topUI + (usableH - dh) / 2;

  if (dx < 0 || dh > usableH) {
    video.closeFile();
    showMessage("Error", "video demasiado grande", COLOR_ERR, 1500);
    return;
  }

  String filename = basenameOf(path);
  uint32_t frameIndex = 0;
  bool paused = false;
  uint32_t targetDelayMs = max<uint32_t>(1, 1000 / video.fps);

  uint32_t lastFpsTick = millis();
  uint16_t renderedFrames = 0;
  uint16_t realFps = 0;

  AudioStreamer audio;
  bool hasAudio = false;

  if (video.audioCodec == 1 && video.audioSize > 0) {
    hasAudio = audio.begin(path,
                           video.audioOffset,
                           video.audioSize,
                           video.audioSampleRate,
                           video.audioChannels,
                           video.audioCodec);
  }

  drawVideoBackground(dx, dy, dw, dh);
  drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

  while (true) {
    uint32_t t0 = millis();
    Action action = readAction();

    if (action == ACT_BACK) {
      break;

    } else if (action == ACT_VOL_DOWN) {
      g_cfg.volume -= 8;
      applyVolume();
      saveConfig();
      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

    } else if (action == ACT_VOL_UP) {
      g_cfg.volume += 8;
      applyVolume();
      saveConfig();
      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

    } else if (action == ACT_BRI_DOWN) {
      g_cfg.brightness -= 16;
      applyBrightness();
      saveConfig();
      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

    } else if (action == ACT_BRI_UP) {
      g_cfg.brightness += 16;
      applyBrightness();
      saveConfig();
      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

    } else if (action == ACT_ENTER) {
      paused = !paused;
      if (paused && hasAudio) {
        M5Cardputer.Speaker.stop(AudioStreamer::CHANNEL);
      } else if (!paused && hasAudio) {
        audio.service();
      }
      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

    } else if (action == ACT_LEFT) {
      uint32_t step = max<uint16_t>(1, video.fps);
      frameIndex = (frameIndex > step) ? (frameIndex - step) : 0;

      if (hasAudio) {
        uint64_t audioBytePos = ((uint64_t)frameIndex * video.audioSampleRate * video.audioChannels) / video.fps;
        audio.seekToByte((uint32_t)audioBytePos);
        audio.service();
      }

      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

    } else if (action == ACT_RIGHT) {
      uint32_t step = max<uint16_t>(1, video.fps);
      frameIndex = min<uint32_t>(video.frameCount - 1, frameIndex + step);

      if (hasAudio) {
        uint64_t audioBytePos = ((uint64_t)frameIndex * video.audioSampleRate * video.audioChannels) / video.fps;
        audio.seekToByte((uint32_t)audioBytePos);
        audio.service();
      }

      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);
    }

    if (!paused) {
      if (!video.seekFrame(frameIndex)) {
        showMessage("ERROR SEEK", "", COLOR_ERR, 1200);
        break;
      }

      bool ok = true;
      int y = 0;

      while (y < video.height) {
        int rows = BLOCK_H;
        if (y + rows > video.height) rows = video.height - y;

        if (!video.readBlockIndexes(rows)) {
          ok = false;
          break;
        }

        video.convertBlockIndexesToRGB(rows);

        M5Cardputer.Display.pushImage(dx, dy + y, video.width, rows, video.blockRgbBuf.data());

        y += rows;
      }

      if (!ok) {
        showMessage("ERROR DIBUJANDO", "", COLOR_ERR, 1200);
        break;
      }

      if (hasAudio) {
        audio.service();
      }

      renderedFrames++;
      uint32_t now = millis();
      if ((now - lastFpsTick) >= 1000) {
        realFps = renderedFrames;
        renderedFrames = 0;
        lastFpsTick = now;
      }

      drawPlayerOverlay(filename, frameIndex, video.frameCount, paused, video.fps, realFps);

      frameIndex++;
      if (frameIndex >= video.frameCount) {
        frameIndex = 0;
        if (hasAudio) {
          audio.reset();
        }
      }
    }

    uint32_t elapsed = millis() - t0;
    if (elapsed < targetDelayMs) {
      delay(targetDelayMs - elapsed);
    }
    yield();
  }

  if (hasAudio) {
    audio.stop();
  }

  video.closeFile();
  showMessage("Fin", "", COLOR_DIM, 300);
}

// ========================================================
// MENUS
// ========================================================
StartMenuResult showStartMenu() {
  int selected = 0;
  bool dirty = true;
  const char* items[2] = {
    "Reproducir CRPTVF",
    "Configuracion"
  };

  while (true) {
    if (dirty) {
      clearScreen(COLOR_BG);
      fillRectSafe(0, 0, SCREEN_W, 14, COLOR_BAR);
      drawText("CRPTVFPlayer", 4, 3, COLOR_FG, COLOR_BAR);

      for (int i = 0; i < 2; ++i) {
        int y = 40 + i * 18;
        if (i == selected) {
          fillRectSafe(10, y - 2, 220, 14, COLOR_SEL);
          drawText(items[i], 14, y, COLOR_BG, COLOR_SEL);
        } else {
          drawText(items[i], 14, y, COLOR_FG, COLOR_BG);
        }
      }

      drawText("W/S mover | E entrar", 10, 110, COLOR_DIM, COLOR_BG);
      dirty = false;
    }

    Action action = readAction();

    if (action == ACT_UP) {
      if (selected > 0) {
        selected--;
        dirty = true;
      }
    } else if (action == ACT_DOWN) {
      if (selected < 1) {
        selected++;
        dirty = true;
      }
    } else if (action == ACT_ENTER || action == ACT_RIGHT) {
      return (selected == 0) ? MENU_PLAY : MENU_CONFIG;
    }

    delay(20);
  }
}

void drawConfigMenu(int selected) {
  clearScreen(COLOR_BG);
  fillRectSafe(0, 0, SCREEN_W, 14, COLOR_BAR);
  drawText("Configuracion", 4, 3, COLOR_FG, COLOR_BAR);

  String lines[6];
  lines[0] = "Carpeta inicio";
  lines[1] = "Tiempo UI: " + String(g_cfg.timeDisplayMode);
  lines[2] = "FPS UI: " + String(g_cfg.fpsDisplayMode);
  lines[3] = "Barras UI: " + String(g_cfg.showBars ? "ON" : "OFF");
  lines[4] = "Volumen: " + String(g_cfg.volume);
  lines[5] = "Brillo: " + String(g_cfg.brightness);

  for (int i = 0; i < 6; ++i) {
    int y = 22 + i * 16;
    if (i == selected) {
      fillRectSafe(2, y - 1, SCREEN_W - 4, 14, COLOR_SEL);
      drawText(lines[i].substring(0, 34), 6, y, COLOR_BG, COLOR_SEL);
    } else {
      drawText(lines[i].substring(0, 34), 6, y, COLOR_FG, COLOR_BG);
    }
  }

  drawText(g_cfg.browserStartPath.substring(0, 34), 6, 120, COLOR_DIM, COLOR_BG);
}

void configMenuLoop() {
  int selected = 0;
  bool dirty = true;

  while (true) {
    if (dirty) {
      drawConfigMenu(selected);
      dirty = false;
    }

    Action action = readAction();

    if (action == ACT_UP) {
      if (selected > 0) {
        selected--;
        dirty = true;
      }
    } else if (action == ACT_DOWN) {
      if (selected < 5) {
        selected++;
        dirty = true;
      }
    } else if (action == ACT_LEFT) {
      if (selected == 1) {
        g_cfg.timeDisplayMode--;
        if (g_cfg.timeDisplayMode < 1) g_cfg.timeDisplayMode = 3;
        saveConfig();
        dirty = true;
      } else if (selected == 2) {
        g_cfg.fpsDisplayMode--;
        if (g_cfg.fpsDisplayMode < 1) g_cfg.fpsDisplayMode = 3;
        saveConfig();
        dirty = true;
      } else if (selected == 3) {
        g_cfg.showBars = !g_cfg.showBars;
        saveConfig();
        dirty = true;
      } else if (selected == 4) {
        g_cfg.volume -= 8;
        applyVolume();
        saveConfig();
        dirty = true;
      } else if (selected == 5) {
        g_cfg.brightness -= 16;
        applyBrightness();
        saveConfig();
        dirty = true;
      } else {
        saveConfig();
        return;
      }
    } else if (action == ACT_RIGHT || action == ACT_ENTER) {
      if (selected == 0) {
        String chosen = browseForFolderAndSave(g_cfg.browserStartPath);
        if (chosen.length() > 0) {
          g_cfg.browserStartPath = chosen;
          applyConfig();
          saveConfig();
        }
        dirty = true;
      } else if (selected == 1) {
        g_cfg.timeDisplayMode++;
        if (g_cfg.timeDisplayMode > 3) g_cfg.timeDisplayMode = 1;
        saveConfig();
        dirty = true;
      } else if (selected == 2) {
        g_cfg.fpsDisplayMode++;
        if (g_cfg.fpsDisplayMode > 3) g_cfg.fpsDisplayMode = 1;
        saveConfig();
        dirty = true;
      } else if (selected == 3) {
        g_cfg.showBars = !g_cfg.showBars;
        saveConfig();
        dirty = true;
      } else if (selected == 4) {
        g_cfg.volume += 8;
        applyVolume();
        saveConfig();
        dirty = true;
      } else if (selected == 5) {
        g_cfg.brightness += 16;
        applyBrightness();
        saveConfig();
        dirty = true;
      }
    } else if (action == ACT_BACK) {
      saveConfig();
      return;
    }

    delay(20);
  }
}

// ========================================================
// APP
// ========================================================
void browserLoop() {
  String currentDir = normalizePath(g_cfg.browserStartPath);
  if (!SD.exists(currentDir)) currentDir = DEFAULT_BROWSER_PATH;
  if (!SD.exists(currentDir)) currentDir = "/";

  int selected = 0;
  int offset = 0;
  bool dirty = true;

  while (true) {
    std::vector<BrowserItem> items = listDirItems(currentDir, true);

    if (selected < 0) selected = 0;
    if (!items.empty() && selected >= (int)items.size()) selected = (int)items.size() - 1;

    if (selected < offset) {
      offset = selected;
      dirty = true;
    }
    if (selected >= offset + VISIBLE_ROWS) {
      offset = selected - VISIBLE_ROWS + 1;
      dirty = true;
    }

    if (dirty) {
      drawBrowser(items, selected, offset, currentDir);
      dirty = false;
    }

    Action action = readAction();

    if (action == ACT_UP) {
      if (selected > 0) {
        selected--;
        dirty = true;
      }
    } else if (action == ACT_DOWN) {
      if (selected < (int)items.size() - 1) {
        selected++;
        dirty = true;
      }
    } else if (action == ACT_ENTER || action == ACT_RIGHT) {
      if (items.empty()) {
        dirty = true;
        continue;
      }

      const BrowserItem& item = items[selected];

      if (item.isDir) {
        currentDir = (item.path == "__PARENT__") ? parentDir(currentDir) : item.path;
        selected = 0;
        offset = 0;
        dirty = true;
      } else {
        showMessage("Abriendo", item.name.substring(0, 28), COLOR_FG, 300);
        playCRPTVF(item.path);
        selected = 0;
        offset = 0;
        dirty = true;
      }
    } else if (action == ACT_BACK || action == ACT_LEFT) {
      if (currentDir != "/") {
        currentDir = parentDir(currentDir);
        selected = 0;
        offset = 0;
        dirty = true;
      } else {
        showMessage("Volviendo...", "", COLOR_DIM, 200);
        break;
      }
    }

    delay(20);
  }
}

// ========================================================
// SETUP / LOOP
// ========================================================
bool appSetup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextFont(nullptr);
  M5Cardputer.Display.setSwapBytes(false);

  clearScreen(COLOR_BG);

  auto spk_cfg = M5Cardputer.Speaker.config();
  spk_cfg.sample_rate = 16000;
  spk_cfg.stereo = false;
  spk_cfg.magnification = 8;
  spk_cfg.dma_buf_len = 512;
  spk_cfg.dma_buf_count = 12;
  M5Cardputer.Speaker.config(spk_cfg);
  M5Cardputer.Speaker.begin();

  if (!initSD()) {
    showMessage("Error SD", "no se pudo iniciar", COLOR_ERR, 0);
    return false;
  }

  ensureAppDir();
  loadConfig();

  return true;
}

void setup() {
  Serial.begin(115200);

  if (appSetup()) {
    while (true) {
      StartMenuResult choice = showStartMenu();
      if (choice == MENU_PLAY) {
        browserLoop();
      } else {
        configMenuLoop();
      }
    }
  }
}

void loop() {
  M5Cardputer.update();
  delay(10);
}
