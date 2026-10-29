#include "text_store.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_partition.h>
#include <string.h>

static Preferences prefs;   // 仅用于读取旧版 NVS 文本; 本固件从不写入/删除旧数据

// ============================================================
// 旧版 NVS (legacy, 只读兼容)
// ============================================================
bool TextStore::legacySlotValid(const LegacyNvsSlot &s) {
  if (s.magic != LEGACY_SLOT_MAGIC) return false;
  if (s.len == 0 || s.len > LEGACY_MAX_TEXT_LEN) return false;  // 旧槽位物理上限 1024
  if (s.crc != proto_crc32(s.data, s.len)) return false;
  return true;
}

bool TextStore::readLegacySlot(const char *key, LegacyNvsSlot &out) {
  static_assert(sizeof(LegacyNvsSlot) == 1040,
                "legacy NVS slot layout must stay byte-identical to v1.0");
  if (!_prefsReady) return false;
  size_t n = prefs.getBytes(key, &out, sizeof(LegacyNvsSlot));
  return n == sizeof(LegacyNvsSlot) && legacySlotValid(out);
}

// ============================================================
// 文件系统挂载: 绝不自动格式化
// ============================================================
// 原始扫描 spiffs 分区是否已含本项目文件头 "HIDF"
// 返回: 1=已发现  0=未发现  -1=扫描错误(视为不安全)
int8_t TextStore::scanHasFileMagic() {
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!p) return -1;
  static uint8_t buf[512 + 3];              // +3 = 跨块匹配的上一块尾部
  size_t have = 0;
  for (uint32_t off = 0; off < p->size; ) {
    size_t n = p->size - off;
    if (n > 512) n = 512;
    if (esp_partition_read(p, off, buf + have, n) != ESP_OK) return -1;
    size_t total = have + n;
    for (size_t i = 0; i + 4 <= total; i++) {
      if (buf[i] == 'H' && buf[i + 1] == 'I' && buf[i + 2] == 'D' && buf[i + 3] == 'F')
        return 1;
    }
    if (total >= 3) { memmove(buf, buf + total - 3, 3); have = 3; }
    else have = total;
    off += n;
  }
  return 0;
}

bool TextStore::mountFilesystem() {
  // 1) 先尝试不自动格式化挂载 — 成功则直接使用, 永不触碰已有文件系统
  //    (SPIFFS.begin(formatOnFail=false) 代码路径上不存在任何 format 调用)
  if (SPIFFS.begin(false, "/spiffs", 10, "spiffs")) {
    _fsMounted = true;
    Serial.println("[fs] spiffs mounted OK (partition spiffs @0x290000, 1441792 B, no format performed)");
    return true;
  }
  // 2) 挂载失败 — 区分原因, 只有全部安全条件满足才做一次性初始化
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!p) {
    Serial.println("[fs] mount failed: spiffs partition NOT FOUND in partition table — refuse to format");
    return false;
  }
  int8_t sc = scanHasFileMagic();
  if (sc < 0) {
    Serial.println("[fs] mount failed: raw partition scan error — cannot confirm absence of existing data, refuse to format");
    return false;
  }
  if (sc > 0) {
    Serial.println("[fs] mount failed: existing project file data detected (fs corrupt?) — refuse to format; legacy NVS still usable");
    return false;
  }
  // 安全条件: 分区存在(data/spiffs, 本项目专属) + 原始扫描无任何本项目数据
  // (升级前该分区实测全 0xFF, 旧固件从不访问) → 允许一次性初始化, 仅格式化本分区
  Serial.println("[fs] partition not formatted and contains no project data → one-time init: format spiffs ONLY (NVS/app untouched)");
  if (!SPIFFS.begin(true, "/spiffs", 10, "spiffs")) {   // format 只作用于 label="spiffs" 分区
    Serial.println("[fs] one-time format failed");
    return false;
  }
  _fsMounted = true;
  Serial.println("[fs] mounted after one-time init");
  return true;
}

bool TextStore::begin() {
  // 旧 NVS: 打开命名空间 (本固件只读, 从不写/删 — 旧文本永远可恢复)
  _prefsReady = prefs.begin("hidtext", false);
  if (!_prefsReady) Serial.println("[store] WARN: legacy NVS namespace unavailable");
  // 文件系统: 不自动格式化挂载 (内部含受控一次性初始化, 失败则降级)
  mountFilesystem();
  return _fsMounted || _prefsReady;
}

// ============================================================
// 槽位文件读取/写入 (20B 文件头 + 正文)
// ============================================================
bool TextStore::readSlotFile(uint8_t slot, FileHdr *hdrOut, uint8_t *contentBuf,
                             const uint8_t *expect) {
  static_assert(sizeof(FileHdr) == 20, "FileHdr must be exactly 20 bytes");
  if (!_fsMounted) return false;
  File f = SPIFFS.open(slotPath(slot), "r");
  if (!f) return false;
  FileHdr h;
  bool ok = (f.read((uint8_t *)&h, sizeof(h)) == sizeof(h));
  if (ok) ok = (h.magic == FILE_MAGIC && h.ver == FILE_VER);
  if (ok) ok = (h.len > 0 && h.len <= MAX_TEXT_LEN);
  if (ok) ok = (f.size() == (uint32_t)(sizeof(h) + h.len));  // 半截/多余字节 → 无效
  if (!ok) { f.close(); return false; }

  uint32_t st = 0xFFFFFFFFu;
  uint8_t buf[512];
  uint32_t got = 0;
  while (got < h.len) {
    size_t want = h.len - got;
    if (want > sizeof(buf)) want = sizeof(buf);
    int n = (int)f.read(buf, want);
    if (n <= 0) { f.close(); return false; }
    st = proto_crc32_cont(st, buf, (size_t)n);
    if (expect && memcmp(buf, expect + got, (size_t)n) != 0) { f.close(); return false; }
    if (contentBuf) memcpy(contentBuf + got, buf, (size_t)n);
    got += (uint32_t)n;
  }
  f.close();
  if (got != h.len) return false;
  if ((st ^ 0xFFFFFFFFu) != h.crc) return false;   // 正文 CRC 校验
  if (hdrOut) *hdrOut = h;
  return true;
}

bool TextStore::writeSlotFile(uint8_t slot, const FileHdr &hdr, const uint8_t *data) {
  if (!_fsMounted) return false;
  // 写入目标恒为“另一”槽位; 当前有效文件不被打开为可写
  File f = SPIFFS.open(slotPath(slot), "w");   // 截断重写
  if (!f) return false;
  if (f.write((const uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }
  if (f.write(data, hdr.len) != hdr.len) { f.close(); return false; }  // 半截文件
  f.close();
  // 回读验证: 重新打开 → 文件头 + 长度 + CRC + 正文逐字节 全部通过才认定有效
  FileHdr rh;
  if (!readSlotFile(slot, &rh, nullptr, data)) return false;
  return (rh.magic == hdr.magic && rh.ver == hdr.ver && rh.seq == hdr.seq &&
          rh.len == hdr.len && rh.crc == hdr.crc);
}

// ============================================================
// 启动加载: FS 优先 → legacy NVS (迁移)
// ============================================================
bool TextStore::loadFromFiles() {
  if (!_fsMounted) return false;
  // 槽 0: 校验并把正文读进 _data (若它最终胜出)
  FileHdr h0, h1;
  int best = -1;
  FileHdr hb;
  if (readSlotFile(0, &h0, _data, nullptr)) { best = 0; hb = h0; }
  // 槽 1: 先只校验 (不碰 _data), 避免覆盖已接受的槽 0 正文
  if (readSlotFile(1, &h1, nullptr, nullptr)) {
    if (best < 0) {
      if (readSlotFile(1, nullptr, _data, nullptr)) { best = 1; hb = h1; }
    } else if (h1.seq > h0.seq) {
      if (readSlotFile(1, nullptr, _data, nullptr)) { best = 1; hb = h1; }
      else {  // 槽 1 校验后读取失败 → 回退重载槽 0
        if (!readSlotFile(0, nullptr, _data, nullptr)) return false;
      }
    }
    // 槽 0 胜出 → _data 保持槽 0 正文
  }
  if (best < 0) return false;   // 两个文件都损坏/不存在

  _lastSlot = (int8_t)best;
  _len = (uint16_t)hb.len;
  _crc = hb.crc;
  _seq = hb.seq;
  _valid = true;
  _usingLegacy = false;
  Serial.printf("[store] loaded from filesystem %s: len=%u crc=%08X seq=%u\n",
                slotPath((uint8_t)best), _len, _crc, (unsigned)_seq);
  return true;
}

bool TextStore::migrateFromLegacy() {
  // 进入时 _data/_len/_crc/_seq 已装载 legacy 内容且 _valid=true
  if (!_fsMounted) return false;
  FileHdr h;
  h.magic = FILE_MAGIC; h.ver = FILE_VER; h.rsvd = 0;
  h.seq = _seq; h.len = _len; h.crc = _crc;
  if (!writeSlotFile(0, h, _data)) {
    Serial.printf("[store] MIGRATION FAILED — legacy text kept active (len=%u crc=%08X seq=%u); NVS untouched\n",
                  _len, _crc, (unsigned)_seq);
    return false;
  }
  _lastSlot = 0;
  _usingLegacy = false;
  Serial.printf("[store] migrated legacy NVS -> /textA.bin VERIFIED: len=%u crc=%08X seq=%u (NVS kept untouched)\n",
                _len, _crc, (unsigned)_seq);
  return true;
}

bool TextStore::load() {
  _valid = false;
  _usingLegacy = false;
  _lastSlot = -1;

  // 1) 优先文件系统新格式
  if (loadFromFiles()) return true;

  // 2) 文件系统无有效文本 → 查找旧版 NVS (独立 Legacy 结构解析)
  static LegacyNvsSlot la, lb;             // 1040B×2, 静态存储 (不占栈)
  bool okA = readLegacySlot("slotA", la);
  bool okB = readLegacySlot("slotB", lb);
  const LegacyNvsSlot *s = nullptr;
  if (okA && okB)      s = (lb.seq >= la.seq) ? &lb : &la;
  else if (okA)        s = &la;
  else if (okB)        s = &lb;
  if (!s) {
    Serial.println("[store] no valid text in filesystem NOR legacy NVS");
    return false;
  }

  memcpy(_data, s->data, s->len);
  _len = s->len;
  _crc = s->crc;
  _seq = s->seq;
  _valid = true;
  _usingLegacy = true;
  strcpy(_lastKey, (s == &la) ? "slotA" : "slotB");

  if (_fsMounted) {
    Serial.printf("[store] filesystem empty/invalid; found legacy NVS %s: len=%u crc=%08X seq=%u — migrating...\n",
                  _lastKey, _len, _crc, (unsigned)_seq);
    migrateFromLegacy();   // 失败时已打印明确错误, _usingLegacy 保持 true
  } else {
    Serial.println("[store] WARNING: filesystem unavailable — running on legacy NVS text "
                   "(read-only fallback, NOT a fresh restore; saves will fail until fs recovers)");
  }
  return true;
}

// ============================================================
// 保存: 写“另一”槽位 + 回读验证; 失败绝不破坏旧文本
// ============================================================
bool TextStore::save(const uint8_t *data, uint16_t len, bool *outChanged) {
  if (outChanged) *outChanged = false;
  if (len == 0) return false;           // 拒绝空文本 (协议层已先行拦截, 此处为纵深防御)
  if (len > MAX_TEXT_LEN) return false;

  uint32_t new_crc = proto_crc32(data, len);

  // 内容完全一致则不写介质 (不无意义递增 seq, 不磨损 Flash)
  if (_valid && _len == len && _crc == new_crc && memcmp(_data, data, len) == 0)
    return true;                        // *outChanged 保持 false

  if (!_fsMounted) {
    Serial.println("[store] SAVE refused: filesystem not mounted — current text kept intact");
    return false;
  }
  uint8_t target = (_lastSlot < 0) ? 0 : (uint8_t)(1 - _lastSlot);  // “另一”槽位

#ifdef TEXT_STORE_FAULT_INJECT
  if (_fiFailMode) {
    uint8_t m = _fiFailMode;
    _fiFailMode = 0;                    // 一次性
    if (m == 2) {                       // 半截: 只写文件头即失败
      FileHdr h;
      h.magic = FILE_MAGIC; h.ver = FILE_VER; h.rsvd = 0;
      h.seq = _valid ? _seq + 1 : 1; h.len = len; h.crc = new_crc;
      File f = SPIFFS.open(slotPath(target), "w");
      if (f) { f.write((const uint8_t *)&h, sizeof(h)); f.close(); }
      Serial.println("[store][FI] injected: partial write (header only) then failure — old text preserved");
    } else {
      Serial.println("[store][FI] injected: failure before any write — nothing written");
    }
    return false;
  }
#endif

  FileHdr h;
  h.magic = FILE_MAGIC;
  h.ver   = FILE_VER;
  h.rsvd  = 0;
  h.seq   = _valid ? (_seq + 1) : 1;
  h.len   = len;
  h.crc   = new_crc;

  if (!writeSlotFile(target, h, data)) {
    Serial.printf("[store] SAVE FAILED writing %s — previous text preserved (len=%u crc=%08X seq=%u)\n",
                  slotPath(target), _len, _crc, (unsigned)_seq);
    return false;
  }

  // 回读验证全部通过 → 才更新 RAM 有效文本 (单次完整拷贝, 无反复复制)
  memcpy(_data, data, len);
  _len = (uint16_t)len;
  _crc = new_crc;
  _seq = h.seq;
  _valid = true;
  _lastSlot = (int8_t)target;
  _usingLegacy = false;
  if (outChanged) *outChanged = true;
  Serial.printf("[store] saved %s: len=%u crc=%08X seq=%u\n",
                slotPath(target), _len, _crc, (unsigned)_seq);
  return true;
}

#ifdef TEXT_STORE_FAULT_INJECT
// ---- 仅故障注入测试构建存在的代码 (生产构建不编译) ----
static int8_t fiResolveSlot(uint8_t which, int8_t lastSlot) {
  switch (which) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return lastSlot;
    case 3:  return (lastSlot >= 0) ? (int8_t)(1 - lastSlot) : (int8_t)-1;
    default: return -1;
  }
}

bool TextStore::fiCorruptHeader(uint8_t which) {
  int8_t slot = fiResolveSlot(which, _lastSlot);
  if (slot < 0 || !_fsMounted) return false;
  File f = SPIFFS.open(slotPath((uint8_t)slot), "r+");
  if (!f) return false;
  uint8_t junk[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  bool ok = f.seek(0) && f.write(junk, sizeof(junk)) == sizeof(junk);
  f.close();
  Serial.printf("[store][FI] corrupted file header of %s (ok=%d)\n", slotPath((uint8_t)slot), ok);
  return ok;
}

bool TextStore::fiFlipContent(uint8_t which) {
  int8_t slot = fiResolveSlot(which, _lastSlot);
  if (slot < 0 || !_fsMounted) return false;
  File f = SPIFFS.open(slotPath((uint8_t)slot), "r+");
  if (!f) return false;
  if (f.size() <= sizeof(FileHdr)) { f.close(); return false; }
  bool ok = f.seek(sizeof(FileHdr));
  uint8_t b = 0;
  ok = ok && (f.read(&b, 1) == 1);
  ok = ok && f.seek(sizeof(FileHdr));
  b ^= 0xFF;
  ok = ok && (f.write(&b, 1) == 1);
  f.close();
  Serial.printf("[store][FI] flipped first content byte of %s (ok=%d) — CRC now invalid\n",
                slotPath((uint8_t)slot), ok);
  return ok;
}

void TextStore::fiFailNextSave(uint8_t mode) {
  _fiFailMode = mode;
  Serial.printf("[store][FI] next save will fail with mode %u\n", mode);
}
#endif
