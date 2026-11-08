// SPDX-License-Identifier: GPL-2.0-or-later
// Port of Dolphin's Core/HW/DSPHLE/UCodes/AX.cpp and AXVoice.h (GameCube AX, ucode 0x4e8a8b21).
#include "ax_ucode.h"
#include <algorithm>
#include <cstring>

namespace ax {
namespace {

Memory g_mem{};
uint64_t g_frames = 0;

// ---- parameter block layout (u16 fields, big-endian in guest RAM) ----
struct PBMixer { uint16_t left, left_delta, right, right_delta, auxA_left, auxA_left_delta, auxA_right, auxA_right_delta,
                 auxB_left, auxB_left_delta, auxB_right, auxB_right_delta, auxB_surround, auxB_surround_delta,
                 surround, surround_delta, auxA_surround, auxA_surround_delta; };
struct PBInitialTimeDelay { uint16_t on, addrMemHigh, addrMemLow, offsetLeft, offsetRight, targetLeft, targetRight; };
struct PBUpdates { uint16_t num_updates[5]; uint16_t data_hi, data_lo; };
struct PBDpop { int16_t left, auxA_left, auxB_left, right, auxA_right, auxB_right, surround, auxA_surround, auxB_surround; };
struct PBVolumeEnvelope { uint16_t cur_volume; int16_t cur_volume_delta; };
struct PBUnknown2 { uint16_t unknown_reserved[3]; };
struct PBAudioAddr { uint16_t looping, sample_format, loop_addr_hi, loop_addr_lo, end_addr_hi, end_addr_lo, cur_addr_hi, cur_addr_lo; };
struct PBADPCMInfo { int16_t coefs[16]; uint16_t gain, pred_scale; int16_t yn1, yn2; };
struct PBSampleRateConverter { uint16_t ratio_hi, ratio_lo, cur_addr_frac; int16_t last_samples[4]; };
struct PBADPCMLoopInfo { uint16_t pred_scale, yn1, yn2; };
struct PBLowPassFilter { uint16_t enabled; int16_t yn1; uint16_t a0, b0; };

struct AXPB {
  uint16_t next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo;
  uint16_t src_type, coef_select, mixer_control, running, is_stream;
  PBMixer mixer;
  PBInitialTimeDelay initial_time_delay;
  PBUpdates updates;
  PBDpop dpop;
  PBVolumeEnvelope vol_env;
  PBUnknown2 unknown3;
  PBAudioAddr audio_addr;
  PBADPCMInfo adpcm;
  PBSampleRateConverter src;
  PBADPCMLoopInfo adpcm_loop_info;
  PBLowPassFilter lpf;
  uint16_t padding[25];
};
static_assert(sizeof(AXPB) == 244, "AXPB layout must match Dolphin's");

enum { SRCTYPE_POLYPHASE = 0, SRCTYPE_LINEAR = 1, SRCTYPE_NEAREST = 2 };
enum {
  MIX_L = 0x000001, MIX_L_RAMP = 0x000002, MIX_R = 0x000004, MIX_R_RAMP = 0x000008, MIX_S = 0x000010, MIX_S_RAMP = 0x000020,
  MIX_AUXA_L = 0x000040, MIX_AUXA_L_RAMP = 0x000080, MIX_AUXA_R = 0x000100, MIX_AUXA_R_RAMP = 0x000200, MIX_AUXA_S = 0x000400, MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L = 0x001000, MIX_AUXB_L_RAMP = 0x002000, MIX_AUXB_R = 0x004000, MIX_AUXB_R_RAMP = 0x008000, MIX_AUXB_S = 0x010000, MIX_AUXB_S_RAMP = 0x020000,
};
enum CmdType {
  CMD_SETUP = 0x00, CMD_DL_AND_VOL_MIX = 0x01, CMD_PB_ADDR = 0x02, CMD_PROCESS = 0x03, CMD_MIX_AUXA = 0x04, CMD_MIX_AUXB = 0x05,
  CMD_UPLOAD_LRS = 0x06, CMD_SET_LR = 0x07, CMD_UNK_08 = 0x08, CMD_MIX_AUXB_NOWRITE = 0x09, CMD_COMPRESSOR_TABLE_ADDR = 0x0A,
  CMD_UNK_0B = 0x0B, CMD_UNK_0C = 0x0C, CMD_MORE = 0x0D, CMD_OUTPUT = 0x0E, CMD_END = 0x0F, CMD_MIX_AUXB_LR = 0x10,
  CMD_SET_OPPOSITE_LR = 0x11, CMD_UNK_12 = 0x12, CMD_SEND_AUX_AND_MIX = 0x13,
};
constexpr uint32_t MAIL_CMDLIST = 0xBABE0000, MAIL_CMDLIST_MASK = 0xFFFF0000;

// 32 samples per millisecond, 5 ms per frame.
int m_samples_left[160], m_samples_right[160], m_samples_surround[160];
int m_samples_auxA_left[160], m_samples_auxA_right[160], m_samples_auxA_surround[160];
int m_samples_auxB_left[160], m_samples_auxB_right[160], m_samples_auxB_surround[160];
uint16_t m_cmdlist[512];
uint32_t m_cmdlist_size = 0;
bool m_next_is_cmdlist = false;
uint16_t m_pending_cmdlist_size = 0;

inline int clamp16(int v) { return std::min(32767, std::max(-32767, v)); }
inline uint32_t hilo(uint16_t hi, uint16_t lo) { return ((uint32_t)hi << 16) | lo; }
inline uint8_t read_aram(uint32_t addr) { return addr < g_mem.aram_size ? g_mem.aram[addr] : 0; }

void read_pb(uint32_t addr, AXPB& pb) {
  uint16_t* dst = (uint16_t*)&pb;
  for (size_t i = 0; i < sizeof(AXPB) / 2; ++i) dst[i] = g_mem.rd16(addr + (uint32_t)i * 2);
}
void write_pb(uint32_t addr, const AXPB& pb) {
  const uint16_t* src = (const uint16_t*)&pb;
  for (size_t i = 0; i < sizeof(AXPB) / 2; ++i) g_mem.wr16(addr + (uint32_t)i * 2, src[i]);
}

// ---- simulated accelerator ----
uint32_t acc_loop_addr, acc_end_addr;
uint32_t* acc_cur_addr;
AXPB* acc_pb;

void accelerator_setup(AXPB* pb, uint32_t* cur_addr) {
  acc_pb = pb;
  acc_loop_addr = hilo(pb->audio_addr.loop_addr_hi, pb->audio_addr.loop_addr_lo);
  acc_end_addr = hilo(pb->audio_addr.end_addr_hi, pb->audio_addr.end_addr_lo);
  acc_cur_addr = cur_addr;
}

uint16_t accelerator_get_sample() {
  uint16_t ret;
  uint8_t step_size_bytes = 0;
  switch (acc_pb->audio_addr.sample_format) {
    case 0x00: {  // ADPCM
      if ((*acc_cur_addr & 15) == 0) {
        acc_pb->adpcm.pred_scale = read_aram((*acc_cur_addr & ~15u) >> 1);
        *acc_cur_addr += 2;
      }
      switch (acc_end_addr & 15) {
        case 0: step_size_bytes = 1; break;
        case 1: step_size_bytes = 0; break;
        default: step_size_bytes = 2; break;
      }
      int scale = 1 << (acc_pb->adpcm.pred_scale & 0xF);
      int coef_idx = (acc_pb->adpcm.pred_scale >> 4) & 0x7;
      int32_t coef1 = acc_pb->adpcm.coefs[coef_idx * 2 + 0];
      int32_t coef2 = acc_pb->adpcm.coefs[coef_idx * 2 + 1];
      int temp = (*acc_cur_addr & 1) ? (read_aram(*acc_cur_addr >> 1) & 0xF) : (read_aram(*acc_cur_addr >> 1) >> 4);
      if (temp >= 8) temp -= 16;
      int val = (scale * temp) + ((0x400 + coef1 * acc_pb->adpcm.yn1 + coef2 * acc_pb->adpcm.yn2) >> 11);
      val = clamp16(val);
      acc_pb->adpcm.yn2 = acc_pb->adpcm.yn1;
      acc_pb->adpcm.yn1 = (int16_t)val;
      *acc_cur_addr += 1;
      ret = (uint16_t)val;
      break;
    }
    case 0x0A:  // 16-bit PCM
      ret = (uint16_t)((read_aram(*acc_cur_addr * 2) << 8) | read_aram(*acc_cur_addr * 2 + 1));
      acc_pb->adpcm.yn2 = acc_pb->adpcm.yn1;
      acc_pb->adpcm.yn1 = (int16_t)ret;
      step_size_bytes = 2;
      *acc_cur_addr += 1;
      break;
    case 0x19:  // 8-bit PCM
      ret = (uint16_t)(read_aram(*acc_cur_addr) << 8);
      acc_pb->adpcm.yn2 = acc_pb->adpcm.yn1;
      acc_pb->adpcm.yn1 = (int16_t)ret;
      step_size_bytes = 2;
      *acc_cur_addr += 1;
      break;
    default:
      return 0;
  }
  // End address reached: loop or stop, as the ucode's accelerator interrupt handler does.
  if (*acc_cur_addr == (acc_end_addr + step_size_bytes - 1)) {
    *acc_cur_addr = acc_loop_addr;
    if (acc_pb->audio_addr.looping) {
      acc_pb->adpcm.pred_scale = acc_pb->adpcm_loop_info.pred_scale;
      if (!acc_pb->is_stream) {
        acc_pb->adpcm.yn1 = (int16_t)acc_pb->adpcm_loop_info.yn1;
        acc_pb->adpcm.yn2 = (int16_t)acc_pb->adpcm_loop_info.yn2;
      }
    } else {
      acc_pb->running = 0;
    }
  }
  return ret;
}

uint32_t resample_audio(int16_t* output, uint32_t count, int16_t* last_samples, uint32_t curr_pos, uint32_t ratio, int srctype) {
  if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE) {
    int16_t temp[4];
    uint32_t idx = 0;
    temp[idx++ & 3] = last_samples[0]; temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2]; temp[idx++ & 3] = last_samples[3];
    for (uint32_t i = 0; i < count; ++i) {
      curr_pos += ratio;
      while (curr_pos >= 0x10000) { temp[idx++ & 3] = (int16_t)accelerator_get_sample(); curr_pos -= 0x10000; }
      uint16_t curr_frac = (uint16_t)(curr_pos & 0xFFFF);
      uint16_t inv_curr_frac = (uint16_t)(-curr_frac);
      int16_t sample;
      if (curr_frac) {
        int32_t s0 = temp[idx++ & 3];
        int32_t s1 = temp[idx++ & 3];
        sample = (int16_t)(((s0 * inv_curr_frac) + (s1 * curr_frac)) >> 16);
        idx += 2;
      } else {
        sample = temp[idx++ & 3];
        idx += 3;
      }
      output[i] = sample;
    }
    last_samples[3] = temp[--idx & 3]; last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3]; last_samples[0] = temp[--idx & 3];
  } else {  // SRCTYPE_NEAREST
    for (uint32_t i = 0; i < count; ++i) output[i] = (int16_t)accelerator_get_sample();
    std::memcpy(last_samples, output + count - 4, 4 * sizeof(int16_t));
  }
  return curr_pos;
}

void get_input_samples(AXPB& pb, int16_t* samples, uint16_t count) {
  uint32_t cur_addr = hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo);
  accelerator_setup(&pb, &cur_addr);
  uint32_t curr_pos = resample_audio(samples, count, pb.src.last_samples, pb.src.cur_addr_frac, hilo(pb.src.ratio_hi, pb.src.ratio_lo), pb.src_type);
  pb.src.cur_addr_frac = (uint16_t)(curr_pos & 0xFFFF);
  pb.audio_addr.cur_addr_hi = (uint16_t)(cur_addr >> 16);
  pb.audio_addr.cur_addr_lo = (uint16_t)(cur_addr & 0xFFFF);
}

void mix_add(int* out, const int16_t* input, uint32_t count, uint16_t* pvol, int16_t* dpop, bool ramp) {
  uint16_t& volume = pvol[0];
  uint16_t volume_delta = ramp ? pvol[1] : 0;
  for (uint32_t i = 0; i < count; ++i) {
    int64_t sample = input[i];
    sample *= volume;
    sample >>= 15;
    sample = clamp16((int32_t)sample);
    out[i] += (int16_t)sample;
    volume += volume_delta;
    *dpop = (int16_t)sample;
  }
}

struct Buffers { int* ptrs[9]; };

void process_voice(AXPB& pb, const Buffers& b, uint16_t count, uint32_t mctrl) {
  if (!pb.running) return;
  int16_t samples[32];
  get_input_samples(pb, samples, count);
  for (uint32_t i = 0; i < count; ++i) {
    samples[i] = (int16_t)clamp16(((int32_t)samples[i] * pb.vol_env.cur_volume) >> 15);
    pb.vol_env.cur_volume += pb.vol_env.cur_volume_delta;
  }
  // (Dolphin keeps the low-pass filter disabled.)
#define MIX_ON(C) (0 != (mctrl & MIX_##C))
#define RAMP_ON(C) (0 != (mctrl & MIX_##C##_RAMP))
  if (MIX_ON(L)) mix_add(b.ptrs[0], samples, count, &pb.mixer.left, &pb.dpop.left, RAMP_ON(L));
  if (MIX_ON(R)) mix_add(b.ptrs[1], samples, count, &pb.mixer.right, &pb.dpop.right, RAMP_ON(R));
  if (MIX_ON(S)) mix_add(b.ptrs[2], samples, count, &pb.mixer.surround, &pb.dpop.surround, RAMP_ON(S));
  if (MIX_ON(AUXA_L)) mix_add(b.ptrs[3], samples, count, &pb.mixer.auxA_left, &pb.dpop.auxA_left, RAMP_ON(AUXA_L));
  if (MIX_ON(AUXA_R)) mix_add(b.ptrs[4], samples, count, &pb.mixer.auxA_right, &pb.dpop.auxA_right, RAMP_ON(AUXA_R));
  if (MIX_ON(AUXA_S)) mix_add(b.ptrs[5], samples, count, &pb.mixer.auxA_surround, &pb.dpop.auxA_surround, RAMP_ON(AUXA_S));
  if (MIX_ON(AUXB_L)) mix_add(b.ptrs[6], samples, count, &pb.mixer.auxB_left, &pb.dpop.auxB_left, RAMP_ON(AUXB_L));
  if (MIX_ON(AUXB_R)) mix_add(b.ptrs[7], samples, count, &pb.mixer.auxB_right, &pb.dpop.auxB_right, RAMP_ON(AUXB_R));
  if (MIX_ON(AUXB_S)) mix_add(b.ptrs[8], samples, count, &pb.mixer.auxB_surround, &pb.dpop.auxB_surround, RAMP_ON(AUXB_S));
#undef MIX_ON
#undef RAMP_ON
}

void apply_updates_for_ms(int curr_ms, uint16_t* pb, const uint16_t* num_updates, uint32_t updates_addr) {
  uint32_t start_idx = 0;
  for (int i = 0; i < curr_ms; ++i) start_idx += num_updates[i];
  for (uint32_t i = start_idx; i < start_idx + num_updates[curr_ms]; ++i) {
    uint16_t update_off = g_mem.rd16(updates_addr + i * 4);
    uint16_t update_val = g_mem.rd16(updates_addr + i * 4 + 2);
    if (update_off < sizeof(AXPB) / 2) pb[update_off] = update_val;
  }
}

void download_and_mix_with_volume(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb) {
  int* buffers_main[3] = {m_samples_left, m_samples_right, m_samples_surround};
  int* buffers_auxa[3] = {m_samples_auxA_left, m_samples_auxA_right, m_samples_auxA_surround};
  int* buffers_auxb[3] = {m_samples_auxB_left, m_samples_auxB_right, m_samples_auxB_surround};
  int** buffers[3] = {buffers_main, buffers_auxa, buffers_auxb};
  uint16_t volumes[3] = {vol_main, vol_auxa, vol_auxb};
  for (uint32_t i = 0; i < 3; ++i) {
    uint32_t ptr = addr;   // Dolphin re-reads from `addr` for each group (pointer reset per iteration)
    uint16_t volume = volumes[i];
    for (uint32_t j = 0; j < 3; ++j) {
      int* buffer = buffers[i][j];
      for (uint32_t k = 0; k < 160; ++k) {
        int64_t sample = (int64_t)(int32_t)g_mem.rd32(ptr); ptr += 4;
        sample *= volume;
        buffer[k] += (int32_t)(sample >> 15);
      }
    }
  }
}

void mix_aux_samples(int aux_id, uint32_t write_addr, uint32_t read_addr) {
  int* buffers[3] = {nullptr, nullptr, nullptr};
  if (aux_id == 0) { buffers[0] = m_samples_auxA_left; buffers[1] = m_samples_auxA_right; buffers[2] = m_samples_auxA_surround; }
  else { buffers[0] = m_samples_auxB_left; buffers[1] = m_samples_auxB_right; buffers[2] = m_samples_auxB_surround; }
  if (write_addr) {
    uint32_t ptr = write_addr;
    for (int* buffer : buffers) for (uint32_t j = 0; j < 160; ++j) { g_mem.wr32(ptr, (uint32_t)buffer[j]); ptr += 4; }
  }
  uint32_t ptr = read_addr;
  for (int& s : m_samples_left) { s += (int)g_mem.rd32(ptr); ptr += 4; }
  for (int& s : m_samples_right) { s += (int)g_mem.rd32(ptr); ptr += 4; }
  for (int& s : m_samples_surround) { s += (int)g_mem.rd32(ptr); ptr += 4; }
}

void upload_lrs(uint32_t dst_addr) {
  uint32_t ptr = dst_addr;
  for (int s : m_samples_left) { g_mem.wr32(ptr, (uint32_t)s); ptr += 4; }
  for (int s : m_samples_right) { g_mem.wr32(ptr, (uint32_t)s); ptr += 4; }
  for (int s : m_samples_surround) { g_mem.wr32(ptr, (uint32_t)s); ptr += 4; }
}

void set_main_lr(uint32_t src_addr) {
  for (uint32_t i = 0; i < 160; ++i) {
    int samp = (int)g_mem.rd32(src_addr + i * 4);
    m_samples_left[i] = samp; m_samples_right[i] = samp; m_samples_surround[i] = 0;
  }
}

void mix_auxb_lr(uint32_t ul_addr, uint32_t dl_addr) {
  uint32_t ptr = ul_addr;
  for (int s : m_samples_auxB_left) { g_mem.wr32(ptr, (uint32_t)s); ptr += 4; }
  for (int s : m_samples_auxB_right) { g_mem.wr32(ptr, (uint32_t)s); ptr += 4; }
  ptr = dl_addr;
  for (uint32_t i = 0; i < 160; ++i) { int samp = (int)g_mem.rd32(ptr); ptr += 4; m_samples_auxB_left[i] = samp; m_samples_left[i] += samp; }
  for (uint32_t i = 0; i < 160; ++i) { int samp = (int)g_mem.rd32(ptr); ptr += 4; m_samples_auxB_right[i] = samp; m_samples_right[i] += samp; }
}

void set_opposite_lr(uint32_t src_addr) {
  for (uint32_t i = 0; i < 160; ++i) {
    int inp = (int)g_mem.rd32(src_addr + i * 4);
    m_samples_left[i] = -inp; m_samples_right[i] = inp; m_samples_surround[i] = 0;
  }
}

void send_aux_and_mix(uint32_t main_auxa_up, uint32_t auxb_s_up, uint32_t main_l_dl, uint32_t main_r_dl, uint32_t auxb_l_dl, uint32_t auxb_r_dl) {
  int* up_buffers[] = {m_samples_auxA_left, m_samples_auxA_right, m_samples_auxA_surround};
  uint32_t ptr = main_auxa_up;
  for (int* up : up_buffers) for (uint32_t j = 0; j < 160; ++j) { g_mem.wr32(ptr, (uint32_t)up[j]); ptr += 4; }
  ptr = auxb_s_up;
  for (int s : m_samples_auxB_surround) { g_mem.wr32(ptr, (uint32_t)s); ptr += 4; }
  int* dl_buffers[] = {m_samples_left, m_samples_right, m_samples_auxB_left, m_samples_auxB_right};
  uint32_t dl_addrs[] = {main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl};
  for (size_t i = 0; i < 4; ++i)
    for (uint32_t j = 0; j < 160; ++j) dl_buffers[i][j] += (int)g_mem.rd32(dl_addrs[i] + j * 4);
}

void copy_cmdlist(uint32_t addr, uint16_t size) {
  if (size >= 512) { m_cmdlist_size = 0; return; }
  for (uint32_t i = 0; i < size; ++i, addr += 2) m_cmdlist[i] = g_mem.rd16(addr);
  m_cmdlist_size = size;
}

void handle_command_list() {
  uint32_t pb_addr = 0;
  uint32_t idx = 0;
  bool end = false;
  int hops = 0, steps = 0;
  auto next = [&]() -> uint16_t { return idx < 512 ? m_cmdlist[idx++] : (uint16_t)CMD_END; };
  while (!end && idx < 512 && steps++ < 4096) {
    uint16_t cmd = next();
    switch (cmd) {
      case CMD_SETUP: { uint16_t hi = next(), lo = next(); setup_processing(hilo(hi, lo)); break; }
      case CMD_DL_AND_VOL_MIX: { uint16_t hi = next(), lo = next(); uint16_t vm = next(), va = next(), vb = next(); download_and_mix_with_volume(hilo(hi, lo), vm, va, vb); break; }
      case CMD_PB_ADDR: { uint16_t hi = next(), lo = next(); pb_addr = hilo(hi, lo); break; }
      case CMD_PROCESS: process_pb_list(pb_addr); break;
      case CMD_MIX_AUXA:
      case CMD_MIX_AUXB: { uint16_t hi = next(), lo = next(), hi2 = next(), lo2 = next(); mix_aux_samples(cmd - CMD_MIX_AUXA, hilo(hi, lo), hilo(hi2, lo2)); break; }
      case CMD_UPLOAD_LRS: { uint16_t hi = next(), lo = next(); upload_lrs(hilo(hi, lo)); break; }
      case CMD_SET_LR: { uint16_t hi = next(), lo = next(); set_main_lr(hilo(hi, lo)); break; }
      case CMD_UNK_08: idx += 10; break;
      case CMD_MIX_AUXB_NOWRITE: { uint16_t hi = next(), lo = next(); mix_aux_samples(1, 0, hilo(hi, lo)); break; }
      case CMD_COMPRESSOR_TABLE_ADDR: idx += 2; break;
      case CMD_UNK_0B: break;
      case CMD_UNK_0C: break;
      case CMD_MORE: { uint16_t hi = next(), lo = next(), size = next(); if (++hops > 16 || size == 0 || size >= 512) { end = true; break; } copy_cmdlist(hilo(hi, lo), size); idx = 0; break; }
      case CMD_OUTPUT: { uint16_t hi = next(), lo = next(), hi2 = next(), lo2 = next(); output_samples(hilo(hi2, lo2), hilo(hi, lo)); break; }
      case CMD_END: end = true; break;
      case CMD_MIX_AUXB_LR: { uint16_t hi = next(), lo = next(), hi2 = next(), lo2 = next(); mix_auxb_lr(hilo(hi, lo), hilo(hi2, lo2)); break; }
      case CMD_SET_OPPOSITE_LR: { uint16_t hi = next(), lo = next(); set_opposite_lr(hilo(hi, lo)); break; }
      case CMD_UNK_12: idx += 4; break;
      case CMD_SEND_AUX_AND_MIX: {
        uint16_t a_hi = next(), a_lo = next(), b_hi = next(), b_lo = next(), c_hi = next(), c_lo = next();
        uint16_t d_hi = next(), d_lo = next(), e_hi = next(), e_lo = next(), f_hi = next(), f_lo = next();
        send_aux_and_mix(hilo(a_hi, a_lo), hilo(b_hi, b_lo), hilo(c_hi, c_lo), hilo(d_hi, d_lo), hilo(e_hi, e_lo), hilo(f_hi, f_lo));
        break;
      }
      default: end = true; break;
    }
  }
  ++g_frames;
}

}  // namespace

void set_memory(const Memory& mem) { g_mem = mem; }
void reset() {
  m_cmdlist_size = 0; m_next_is_cmdlist = false; m_pending_cmdlist_size = 0; g_frames = 0;
  int* all[] = {m_samples_left, m_samples_right, m_samples_surround, m_samples_auxA_left, m_samples_auxA_right, m_samples_auxA_surround, m_samples_auxB_left, m_samples_auxB_right, m_samples_auxB_surround};
  for (int* b : all) std::memset(b, 0, 160 * sizeof(int));
}
uint64_t frames_processed() { return g_frames; }

uint32_t convert_mixer_control(uint16_t mixer_control) {
  // ucode 0x4e8a8b21 mapping (Dolphin AXUCode::ConvertMixerControl).
  uint32_t ret = MIX_L | MIX_R;
  if (mixer_control & 0x0001) ret |= MIX_AUXA_L | MIX_AUXA_R;
  if (mixer_control & 0x0002) ret |= MIX_AUXB_L | MIX_AUXB_R;
  if (mixer_control & 0x0004) {
    ret |= MIX_S;
    if (ret & MIX_AUXA_L) ret |= MIX_AUXA_S;
    if (ret & MIX_AUXB_L) ret |= MIX_AUXB_S;
  }
  if (mixer_control & 0x0008) {
    ret |= MIX_L_RAMP | MIX_R_RAMP;
    if (ret & MIX_AUXA_L) ret |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (ret & MIX_AUXB_L) ret |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (ret & MIX_AUXA_S) ret |= MIX_AUXA_S_RAMP;
    if (ret & MIX_AUXB_S) ret |= MIX_AUXB_S_RAMP;
  }
  return ret;
}

void setup_processing(uint32_t init_addr) {
  uint16_t init_data[0x20];
  for (uint32_t i = 0; i < 0x20; ++i) init_data[i] = g_mem.rd16(init_addr + 2 * i);
  int* buffers[] = {m_samples_left, m_samples_right, m_samples_surround, m_samples_auxA_left, m_samples_auxA_right, m_samples_auxA_surround, m_samples_auxB_left, m_samples_auxB_right, m_samples_auxB_surround};
  uint32_t init_idx = 0;
  for (int* buffer : buffers) {
    int32_t init_val = (int32_t)(((uint32_t)init_data[init_idx] << 16) | init_data[init_idx + 1]);
    int16_t delta = (int16_t)init_data[init_idx + 2];
    init_idx += 3;
    if (!init_val) std::memset(buffer, 0, 160 * sizeof(int));
    else for (uint32_t j = 0; j < 160; ++j) { buffer[j] = init_val; init_val += delta; }
  }
}

void process_pb_list(uint32_t pb_addr) {
  const uint32_t spms = 32;
  AXPB pb;
  int guard = 0;
  while (pb_addr && guard++ < 256) {
    Buffers buffers{{m_samples_left, m_samples_right, m_samples_surround, m_samples_auxA_left, m_samples_auxA_right, m_samples_auxA_surround, m_samples_auxB_left, m_samples_auxB_right, m_samples_auxB_surround}};
    read_pb(pb_addr, pb);
    uint32_t updates_addr = hilo(pb.updates.data_hi, pb.updates.data_lo);
    for (int curr_ms = 0; curr_ms < 5; ++curr_ms) {
      apply_updates_for_ms(curr_ms, (uint16_t*)&pb, pb.updates.num_updates, updates_addr);
      process_voice(pb, buffers, (uint16_t)spms, convert_mixer_control(pb.mixer_control));
      for (int*& p : buffers.ptrs) p += spms;
    }
    write_pb(pb_addr, pb);
    pb_addr = hilo(pb.next_pb_hi, pb.next_pb_lo);
  }
}

void output_samples(uint32_t lr_addr, uint32_t surround_addr) {
  for (uint32_t i = 0; i < 160; ++i) g_mem.wr32(surround_addr + i * 4, (uint32_t)m_samples_surround[i]);
  // Clamped 16-bit samples interleaved R L R L ... (AI DMA order).
  for (uint32_t i = 0; i < 160; ++i) {
    int left = clamp16(m_samples_left[i]), right = clamp16(m_samples_right[i]);
    g_mem.wr16(lr_addr + i * 4, (uint16_t)right);
    g_mem.wr16(lr_addr + i * 4 + 2, (uint16_t)left);
  }
}

void handle_mail(uint32_t mail) {
  if (m_next_is_cmdlist) {
    m_next_is_cmdlist = false;
    copy_cmdlist(mail, m_pending_cmdlist_size);
    handle_command_list();
    m_cmdlist_size = 0;
    return;
  }
  if ((mail & MAIL_CMDLIST_MASK) == MAIL_CMDLIST) {
    m_next_is_cmdlist = true;
    m_pending_cmdlist_size = (uint16_t)(mail & ~MAIL_CMDLIST_MASK);
  }
  // MAIL_RESUME / MAIL_CONTINUE / MAIL_RESET need no action: the DSP acknowledges instantly here.
}

}  // namespace ax
