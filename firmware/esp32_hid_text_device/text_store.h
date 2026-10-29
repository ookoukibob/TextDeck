#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

// ============================================================
// 存储架构 (10 KiB 升级):
//
// 新格式 (主存储): SPIFFS 文件双槽位 /textA.bin, /textB.bin
//   分区: default.csv 的 "spiffs" (0x290000, 1,441,792 B, 本项目专用,
//         旧固件从未使用, 当前全 0xFF)。不修改分区表。
//   文件头 FileHdr (20B, 小端): magic "HIDF" + ver + rsvd + seq u32
//                                 + len u32 + crc32 u32, 之后是正文。
//   保存 = 写入“另一”槽位 → 关闭 → 重开回读(头/长度/CRC/正文) → 全部
//         通过才成为新版本; 失败/掉电不破坏旧有效文件。
//   挂载: SPIFFS.begin(false) 不自动格式化; 挂载失败时先确认分区存在、
//         且原始扫描未发现本项目文件数据, 才做一次性初始化(仅 spiffs 分区)。
//
// 旧格式 (只读兼容 + 迁移源): NVS Preferences 命名空间 "hidtext",
//   键 slotA/slotB, LegacyNvsSlot 与旧固件 1040B 布局逐字节一致。
//   迁移: FS 无有效文本而 legacy 有效时 → 写入 FS + 回读验证 → 才算成功;
//   迁移成功前后都不删除/不覆写 NVS; 迁移失败保留 legacy 并明确告警。
// ============================================================

#define LEGACY_MAX_TEXT_LEN 1024   // 旧版上限 (仅用于 legacy NVS 兼容, 不得改)

class TextStore {
public:
  // 打开 NVS + 挂载文件系统(受控, 见上)。返回 false = 两者都不可用
  bool begin();
  // 恢复有效文本: FS 优先 → 否则 legacy NVS 迁移 → 否则 invalid
  bool load();

  bool valid() const { return _valid; }
  const uint8_t *data() const { return _data; }
  uint16_t length() const { return _len; }
  uint32_t crc() const { return _crc; }
  uint32_t seq() const { return _seq; }
  bool usingLegacy() const { return _usingLegacy; }  // 当前文本是否来自旧 NVS (降级/迁移未完成)
  bool fsMounted() const { return _fsMounted; }

  // 保存新文本。outChanged=false 表示内容与当前完全一致，未产生 Flash 写入
  bool save(const uint8_t *data, uint16_t len, bool *outChanged);

#ifdef TEXT_STORE_FAULT_INJECT
  // ---- 仅故障注入测试构建编译 (生产构建无此接口) ----
  // which: 0=textA 1=textB 2=active 3=inactive(另一槽)
  bool fiCorruptHeader(uint8_t which);  // 覆盖文件头 magic → 格式校验失败
  bool fiFlipContent(uint8_t which);    // 翻转正文 1 字节 → CRC 校验失败
  void fiFailNextSave(uint8_t mode);    // 1=写前失败 2=只写头后失败(半截文件)
  // 只读: 原始扫描 spiffs 分区是否已含本项目数据 (1=已发现 0=无 -1=错误)
  // 用于验证“挂载失败时, 已存在项目数据 → 拒绝格式化”这一前置判据, 不修改任何数据
  int8_t fiScanMagic() { return scanHasFileMagic(); }
  int8_t fiLastSlot() const { return _lastSlot; }      // 当前 active FS 槽位 (-1/0/1)
  uint8_t fiFailMode() const { return _fiFailMode; }   // 待触发的失败模式 (0=无)
#endif

private:
  static const uint32_t FILE_MAGIC = 0x46444948;  // "HIDF" (小端: H I D F)
  static const uint16_t FILE_VER   = 1;

  struct FileHdr {           // 20 字节, 小端 (ESP32 与桌面均小端)
    uint32_t magic;          // FILE_MAGIC
    uint16_t ver;            // FILE_VER
    uint16_t rsvd;           // 0
    uint32_t seq;            // 版本号 (每次实际写入 +1)
    uint32_t len;            // 正文字节数 (1..MAX_TEXT_LEN)
    uint32_t crc;            // 正文 CRC32
  } __attribute__((packed));

  // 旧 NVS 槽位 — 与升级前固件逐字节一致 (magic, seq, len, rsvd, crc, data[1024])
  // 禁止修改布局: 旧数据必须用该结构解析
  static const uint32_t LEGACY_SLOT_MAGIC = 0x54444948;  // "HIDT"
  struct LegacyNvsSlot {
    uint32_t magic;
    uint32_t seq;
    uint16_t len;
    uint16_t rsvd;
    uint32_t crc;                 // data[0..len) 的 CRC32
    uint8_t  data[LEGACY_MAX_TEXT_LEN];
  } __attribute__((packed));

  static bool legacySlotValid(const LegacyNvsSlot &s);
  bool readLegacySlot(const char *key, LegacyNvsSlot &out);

  bool mountFilesystem();         // 不自动格式化; 受控一次性初始化 (见头注释)
  int8_t scanHasFileMagic();      // 原始扫描: 1=已发现本项目数据 0=无 -1=错误

  const char *slotPath(uint8_t slot) const { return slot == 0 ? "/textA.bin" : "/textB.bin"; }
  // 读取并校验槽位文件 (头 magic/ver/len、大小、正文 CRC)。
  //   contentBuf != nullptr → 正文读入该缓冲 (大小 MAX_TEXT_LEN)
  //   expect != nullptr     → 逐块与 expect 比对正文 (回读验证)
  //   hdrOut                 → 返回文件头
  bool readSlotFile(uint8_t slot, FileHdr *hdrOut, uint8_t *contentBuf,
                    const uint8_t *expect);
  // 写入槽位文件 (先截断重写) 并立即回读验证; 失败返回 false
  bool writeSlotFile(uint8_t slot, const FileHdr &hdr, const uint8_t *data);

  bool loadFromFiles();           // 双文件选 seq 最大且有效者
  bool migrateFromLegacy();       // legacy → FS (回读验证通过才算成功)

  uint8_t _data[MAX_TEXT_LEN];    // 当前有效文本 RAM 副本 (静态, 不在栈上)
  uint16_t _len = 0;
  uint32_t _crc = 0;
  uint32_t _seq = 0;
  bool _valid = false;
  bool _prefsReady = false;
  bool _fsMounted = false;
  bool _usingLegacy = false;
  int8_t  _lastSlot = -1;         // 最近成功写入/加载的 FS 槽位 (0/1)
  char    _lastKey[6] = "slotA";  // legacy 最近槽位 (迁移溯源用)
#ifdef TEXT_STORE_FAULT_INJECT
  uint8_t _fiFailMode = 0;        // 1/2 = 下次 save 按指定方式失败 (一次性)
#endif
};
