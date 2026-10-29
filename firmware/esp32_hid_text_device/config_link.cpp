#include <Arduino.h>
#include <stdarg.h>
#include "config_link.h"

// 由主程序在 loop() 中更新: 设备侧 USB HID 当前是否已挂载 (供 PING 返回)
uint8_t s_hidReadyState = 0;

// 响应帧缓冲: header + 最大 payload = 10 + 10253 = 10263 B。
// 静态存储 (不在栈上); 两个 ConfigLink 实例顺序调用, 不会并发。
static uint8_t s_out[PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD];

void ConfigLink::begin(TextStore *store, Stream *io, HidTyper *typer, bool usbPort) {
  _store = store;
  _io = io;
  _typer = typer;
  _usbPort = usbPort;
  resetParser();
}

void ConfigLink::resetParser() {
  _state = S_SYNC0;
  _crcIdx = 0;
  _got = 0;
}

void ConfigLink::resync(uint8_t b) {
  _state = (b == PROTO_MAGIC0) ? S_SYNC1 : S_SYNC0;
  _crcIdx = 0;
  _got = 0;
}

// 调试日志永远走 UART0 (Serial), 绝不写入 USB CDC 协议端口。
void ConfigLink::logLine(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

void ConfigLink::poll() {
  while (_io && _io->available() > 0) {
    int rv = _io->read();
    if (rv < 0) break;
    uint8_t b = (uint8_t)rv;
    switch (_state) {
      case S_SYNC0:
        if (b == PROTO_MAGIC0) _state = S_SYNC1;
        break;
      case S_SYNC1:
        if (b == PROTO_MAGIC1) _state = S_VER;
        else if (b != PROTO_MAGIC0) _state = S_SYNC0;
        break;
      case S_VER:
        if (b == PROTO_VERSION) _state = S_CMD;
        else resync(b);
        break;
      case S_CMD:
        _cmd = b;
        _state = S_LEN0;
        break;
      case S_LEN0:
        _len = b;
        _state = S_LEN1;
        break;
      case S_LEN1:
        _len |= (uint16_t)b << 8;
        if (_len > PROTO_MAX_PAYLOAD) {          // 帧长度非法: 静默重新同步
          logLine("[link] frame too long, resync\n");
          resetParser();
        } else {
          _crcIdx = 0;
          _state = S_CRC;
        }
        break;
      case S_CRC:
        _crcBytes[_crcIdx++] = b;
        if (_crcIdx == 4) {
          _crc = get_u32(_crcBytes);
          _got = 0;
          // 空 payload 的帧 (PING/START/CANCEL/GET_STATUS) 在这里就已经完整,
          // 必须立即处理; 若留到"下一个字节"再进入 S_CHECK, 响应会一直压到
          // 主机下次发数据时才发出 (实测表现为响应延迟一帧)。
          if (_len == 0) {
            finishFrame();
          } else {
            _state = S_PAYLOAD;
          }
        }
        break;
      case S_PAYLOAD:
        _payload[_got++] = b;
        if (_got == _len) finishFrame();
        break;
    }
  }
}

// 校验并分发当前已完整接收的帧, 然后复位解析器。
void ConfigLink::finishFrame() {
  if (proto_crc32(_payload, _len) == _crc) {
    _frameCount++;
    // Any valid request refreshes the host lease before the command runs,
    // so a GET_STATUS poll keeps a long typing task alive.
    if (_typer) _typer->noteHostContact();
    handleFrame(_cmd, _payload, _len);
  } else {
    logLine("[link] CRC mismatch, NAK\n");
    uint8_t pl[14] = {ST_ERR_CRC};
    sendFrame(_cmd | 0x80, pl, sizeof(pl));
  }
  resetParser();
}

void ConfigLink::sendFrame(uint8_t cmd, const uint8_t *payload, uint16_t len) {
  s_out[0] = PROTO_MAGIC0;
  s_out[1] = PROTO_MAGIC1;
  s_out[2] = PROTO_VERSION;
  s_out[3] = cmd;
  put_u16(&s_out[4], len);
  put_u32(&s_out[6], proto_crc32(payload, len));
  if (len) memcpy(&s_out[PROTO_HEADER_LEN], payload, len);
  // 整帧一次性写出，避免日志插入帧中间。
  if (!_io) return;
  const size_t total = (size_t)PROTO_HEADER_LEN + len;
  size_t wrote = _io->write(s_out, total);
  // 显式 flush: USBCDC 只有把 TinyUSB TX FIFO 刷到 IN 端点后才真正发出,
  // UART 则等待移位寄存器排空, 两种链路都能保证响应在本次 loop 内离机。
  _io->flush();

  if (wrote != total) {
    logLine("[tx] short write cmd=0x%02X wrote=%u/%u\n", cmd,
            (unsigned)wrote, (unsigned)total);
  }
}

// 统一 16 字节状态响应。布局见 protocol.h 的 TY_STATUS_PAYLOAD_LEN 注释。
void ConfigLink::fillTypingStatus(uint8_t *pl, uint8_t requestStatus) {
  memset(pl, 0, TY_STATUS_PAYLOAD_LEN);
  HidTyper::Status st = {};
  if (_typer) st = _typer->status();
  pl[0] = requestStatus;
  pl[1] = st.state;
  put_u16(&pl[2], st.processed);
  put_u16(&pl[4], st.total);
  pl[6] = st.lastError;
  pl[7] = st.cancelReason;
  pl[8] = st.flags;
  pl[9] = 0;
  put_u16(&pl[10], st.leaseRemainingMs);
  put_u16(&pl[12], st.sessionId);
  put_u16(&pl[14], FW_CAPABILITIES);
}

// WRITE 响应 payload (14B): status, detail u16, saved_len u16, saved_crc u32, seq u32, written u8
// READ  响应 payload (13B+文本): status, detail u16, saved_len u16, saved_crc u32, seq u32, text[]
// PING  响应 payload (16B): status, proto_ver, fw_ver u16, max_text u16, has_text,
//        saved_len u16, saved_crc u32, seq 低16位 u16 (偏移13), hid_ready u8 (偏移15)
// START/CANCEL/GET_STATUS 响应 payload 固定 16B: 见 fillTypingStatus / protocol.h
void ConfigLink::handleFrame(uint8_t cmd, const uint8_t *payload, uint16_t len) {
  switch (cmd) {
    case CMD_WRITE_TEXT: {
      uint8_t pl[14] = {0};
      uint16_t detail = 0;

      // 长度校验先行: 空文本 (len==0) 与超长一律按长度非法拒绝。
      // 拒绝时不触碰任何存储介质: 不写槽位、不递增 seq、不破坏已存文本;
      // 响应中的 saved_len/saved_crc/seq 始终取自设备当前保存状态。
      if (len == 0 || len > MAX_TEXT_LEN) {
        pl[0] = ST_ERR_LENGTH;
      } else if (_typer && _typer->isActive()) {
        // 输入任务持有 TextStore::_data 的只读引用; 输入期间改写文本会让
        // 状态机读到新内容, 因此明确拒绝并保持现有文本不变。
        pl[0] = ST_ERR_BUSY;
      } else {
        for (uint16_t i = 0; i < len; i++) {
          if (!proto_char_supported(payload[i])) {
            pl[0] = ST_ERR_CHARSET;
            detail = i;
            break;
          }
        }
        if (pl[0] == ST_OK) {
          bool changed = false;
          if (_store && _store->save(payload, len, &changed)) {
            pl[0] = ST_OK;
            pl[13] = changed ? 1 : 0;
          } else {
            pl[0] = ST_ERR_STORAGE;
          }
        }
      }

      put_u16(&pl[1], detail);
      put_u16(&pl[3], _store ? _store->length() : 0);
      put_u32(&pl[5], _store ? _store->crc() : 0);
      put_u32(&pl[9], _store ? _store->seq() : 0);
      sendFrame(cmd | 0x80, pl, sizeof(pl));

      logLine("[link] WRITE status=%u detail=%u saved_len=%u crc=%08X seq=%u written=%u\n",
              pl[0], detail, _store ? _store->length() : 0,
              _store ? _store->crc() : 0, _store ? _store->seq() : 0, pl[13]);
      break;
    }

    case CMD_READ_TEXT: {
      // 10253 B 静态存储 (不在栈上): READ 响应 = 13 B 元数据 + 文本
      static uint8_t pl[13 + MAX_TEXT_LEN];
      uint8_t status = ST_OK;
      uint16_t tlen = (_store && _store->valid()) ? _store->length() : 0;
      pl[0] = status;
      put_u16(&pl[1], 0);
      put_u16(&pl[3], tlen);
      put_u32(&pl[5], (_store && _store->valid()) ? _store->crc() : 0);
      put_u32(&pl[9], (_store && _store->valid()) ? _store->seq() : 0);
      if (tlen && _store) memcpy(&pl[13], _store->data(), tlen);
      sendFrame(cmd | 0x80, pl, (uint16_t)(13 + tlen));
      logLine("[link] READ len=%u crc=%08X seq=%u\n", tlen,
              (_store && _store->valid()) ? _store->crc() : 0,
              (_store && _store->valid()) ? _store->seq() : 0);
      break;
    }

    case CMD_PING: {
      uint8_t pl[16] = {0};
      pl[0] = ST_OK;
      pl[1] = PROTO_VERSION;
      put_u16(&pl[2], FW_VERSION);
      put_u16(&pl[4], MAX_TEXT_LEN);
      bool has = (_store && _store->valid());
      pl[6] = has ? 1 : 0;
      put_u16(&pl[7], has ? _store->length() : 0);
      put_u32(&pl[9], has ? _store->crc() : 0);
      put_u16(&pl[13], has ? (uint16_t)(_store->seq() & 0xFFFF) : 0);
      pl[15] = s_hidReadyState;   // 设备侧当前 HID 是否挂载 (见主程序轮询更新)
      sendFrame(cmd | 0x80, pl, sizeof(pl));
      break;
    }

    case CMD_START_TYPING: {
      uint8_t pl[TY_STATUS_PAYLOAD_LEN] = {0};
      uint8_t reqStatus = ST_OK;

      if (!_store || !_store->valid() || _store->length() == 0) {
        reqStatus = ST_ERR_NO_TEXT;
      } else if (_typer && _typer->isActive()) {
        reqStatus = ST_ERR_BUSY;
      } else if (!_typer || s_hidReadyState == 0) {
        reqStatus = ST_ERR_HID_NOT_READY;
      } else {
        HidTyper::StartResult r = _typer->beginTypeText(
            _store->data(), _store->length(), true);
        switch (r) {
          case HidTyper::START_OK:             reqStatus = ST_OK; break;
          case HidTyper::START_BUSY:           reqStatus = ST_ERR_BUSY; break;
          case HidTyper::START_NO_TEXT:        reqStatus = ST_ERR_NO_TEXT; break;
          case HidTyper::START_HID_NOT_READY:  reqStatus = ST_ERR_HID_NOT_READY; break;
          case HidTyper::START_INVALID:
          default:                             reqStatus = ST_ERR_CHARSET; break;
        }
      }
      fillTypingStatus(pl, reqStatus);
      sendFrame(cmd | 0x80, pl, sizeof(pl));
      logLine("[link] START status=%u state=%u len=%u\n", pl[0], pl[1],
              (unsigned)get_u16(&pl[4]));
      break;
    }

    case CMD_CANCEL_TYPING: {
      // cancel() 在返回前同步执行 releaseAll(); 响应在释放动作之后发送,
      // 因此主机收到 ST_OK 即可确认"停止已执行", 而不仅是"命令已排队"。
      uint8_t pl[TY_STATUS_PAYLOAD_LEN] = {0};
      if (_typer) _typer->cancel(CANCEL_HOST);
      fillTypingStatus(pl, ST_OK);
      sendFrame(cmd | 0x80, pl, sizeof(pl));
      logLine("[link] CANCEL state=%u processed=%u/%u reason=%u\n", pl[1],
              (unsigned)get_u16(&pl[2]), (unsigned)get_u16(&pl[4]), pl[7]);
      break;
    }

    case CMD_GET_STATUS: {
      uint8_t pl[TY_STATUS_PAYLOAD_LEN] = {0};
      fillTypingStatus(pl, ST_OK);
      sendFrame(cmd | 0x80, pl, sizeof(pl));
      break;
    }

#ifdef TEXT_STORE_FAULT_INJECT
    // ---- 仅故障注入测试构建编译; 生产构建无此 case (0x7E → default/ST_ERR_CMD) ----
    // payload: [0]=op 1=破坏文件头 2=翻转正文(CRC失效) 3=下次保存写前失败
    //                4=下次保存半截失败 5=清除失败注入
    //                6=只读探测: 原始扫描 spiffs 分区是否已含本项目数据
    //                  ([2]=scan+1: 0=错误 1=未发现 2=已发现; [3]=fsMounted)
    //          [1]=slot 0=textA 1=textB 2=active 3=inactive (可省略, 默认 2)
    case 0x7E: {
      uint8_t pl[4] = {ST_OK, 0, 0, 0};
      if (len < 1) {
        pl[0] = ST_ERR_LENGTH;
      } else {
        uint8_t op = payload[0];
        uint8_t which = (len >= 2) ? payload[1] : 2;
        pl[1] = op;
        if (op == 0) {                     // 状态查询: [2]=lastSlot+1, [3]=failMode
          pl[2] = (uint8_t)(_store ? (_store->fiLastSlot() + 1) : 0);
          pl[3] = _store ? _store->fiFailMode() : 0;
          sendFrame(cmd | 0x80, pl, sizeof(pl));
          logLine("[link] FI status lastSlot=%d failMode=%u\n",
                  _store ? _store->fiLastSlot() : -1,
                  _store ? _store->fiFailMode() : 0);
          break;
        }
        bool ok = false;
        switch (op) {
          case 1: ok = _store && _store->fiCorruptHeader(which); break;
          case 2: ok = _store && _store->fiFlipContent(which); break;
          case 3: if (_store) { _store->fiFailNextSave(1); ok = true; } break;
          case 4: if (_store) { _store->fiFailNextSave(2); ok = true; } break;
          case 5: if (_store) { _store->fiFailNextSave(0); ok = true; } break;
          case 6: {                      // 只读探测, 不改动任何数据
            int8_t sc = _store ? _store->fiScanMagic() : (int8_t)-1;
            pl[2] = (uint8_t)(sc + 1);   // 0=错误 1=未发现项目数据 2=已发现项目数据
            pl[3] = (_store && _store->fsMounted()) ? 1 : 0;
            ok = (sc >= 0);
            break;
          }
          default: ok = false; break;
        }
        if (op != 6) pl[2] = ok ? 1 : 0;
        if (pl[0] == ST_OK && !ok) pl[0] = ST_ERR_STORAGE;
      }
      sendFrame(cmd | 0x80, pl, sizeof(pl));
      logLine("[link] FI(op-test-only) op=%u result=%u status=%u\n", pl[1], pl[2], pl[0]);
      break;
    }
#endif

    default: {
      uint8_t pl[14] = {ST_ERR_CMD};
      sendFrame(cmd | 0x80, pl, sizeof(pl));
      logLine("[link] unknown cmd 0x%02X\n", cmd);
      break;
    }
  }
}
