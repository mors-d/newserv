#include "QuestScript.hh"

#include <stdint.h>
#include <string.h>

#include <array>
#include <deque>
#include <map>
#include <phosg/Math.hh>
#include <phosg/Strings.hh>
#include <set>
#include <unordered_map>
#include <vector>

#include "CommandFormats.hh"
#include "Compression.hh"
#include "StaticGameData.hh"

using namespace std;

template <>
const char* name_for_enum<QuestScriptVersion>(QuestScriptVersion v) {
  switch (v) {
    case QuestScriptVersion::DC_NTE:
      return "DC_NTE";
    case QuestScriptVersion::DC_V1:
      return "DC_V1";
    case QuestScriptVersion::DC_V2:
      return "DC_V2";
    case QuestScriptVersion::PC_V2:
      return "PC_V2";
    case QuestScriptVersion::GC_NTE:
      return "GC_NTE";
    case QuestScriptVersion::GC_V3:
      return "GC_V3";
    case QuestScriptVersion::XB_V3:
      return "XB_V3";
    case QuestScriptVersion::GC_EP3:
      return "GC_EP3";
    case QuestScriptVersion::BB_V4:
      return "BB_V4";
    default:
      return "__UNKNOWN__";
  }
}

// bit_cast isn't in the standard place on macOS (it is apparently implicitly
// included by resource_dasm, but newserv can be built without resource_dasm)
// and I'm too lazy to go find the right header to include
template <typename ToT, typename FromT>
ToT as_type(const FromT& v) {
  static_assert(sizeof(FromT) == sizeof(ToT), "types are not the same size");
  ToT ret;
  memcpy(&ret, &v, sizeof(ToT));
  return ret;
}

static string format_and_indent_data(const void* data, size_t size, uint64_t start_address) {
  struct iovec iov;
  iov.iov_base = const_cast<void*>(data);
  iov.iov_len = size;

  string ret = "  ";
  format_data(
      [&ret](const void* vdata, size_t size) -> void {
        const char* data = reinterpret_cast<const char*>(vdata);
        for (size_t z = 0; z < size; z++) {
          if (data[z] == '\n') {
            ret += "\n  ";
          } else {
            ret.push_back(data[z]);
          }
        }
      },
      &iov, 1, start_address, nullptr, 0, PrintDataFlags::PRINT_ASCII);

  strip_trailing_whitespace(ret);
  return ret;
}

static string dasm_u16string(const char16_t* data, size_t size) {
  try {
    return format_data_string(encode_sjis(data, size));
  } catch (const runtime_error& e) {
    return "/* undecodable */ " + format_data_string(data, size * sizeof(char16_t));
  }
}

template <size_t Size>
static string dasm_u16string(const parray<char16_t, Size>& data) {
  return dasm_u16string(data.data(), data.size());
}

struct ResistData {
  le_int16_t evp_bonus;
  le_uint16_t unknown_a1;
  le_uint16_t unknown_a2;
  le_uint16_t unknown_a3;
  le_uint16_t unknown_a4;
  le_uint16_t unknown_a5;
  le_uint32_t unknown_a6;
  le_uint32_t unknown_a7;
  le_uint32_t unknown_a8;
  le_uint32_t unknown_a9;
  le_int32_t dfp_bonus;
} __attribute__((packed));

struct AttackData {
  le_int16_t unknown_a1;
  le_int16_t unknown_a2;
  le_uint16_t unknown_a3;
  le_uint16_t unknown_a4;
  le_float unknown_a5;
  le_uint32_t unknown_a6;
  le_float unknown_a7;
  le_uint16_t unknown_a8;
  le_uint16_t unknown_a9;
  le_uint16_t unknown_a10;
  le_uint16_t unknown_a11;
  le_uint32_t unknown_a12;
  le_uint32_t unknown_a13;
  le_uint32_t unknown_a14;
  le_uint32_t unknown_a15;
  le_uint32_t unknown_a16;
} __attribute__((packed));

struct MovementData {
  parray<le_float, 6> unknown_a1;
  parray<le_float, 6> unknown_a2;
} __attribute__((packed));

struct UnknownF8F2Entry {
  parray<le_float, 4> unknown_a1;
} __attribute__((packed));

struct QuestScriptOpcodeDefinition {
  struct Argument {
    enum class Type {
      LABEL16 = 0,
      LABEL16_SET,
      LABEL32,
      REG,
      REG_SET,
      REG_SET_FIXED, // Sequence of N consecutive regs
      REG32,
      REG32_SET_FIXED, // Sequence of N consecutive regs
      INT8,
      INT16,
      INT32,
      FLOAT32,
      CSTRING,
    };

    enum class DataType {
      NONE = 0,
      SCRIPT,
      DATA,
      CSTRING,
      PLAYER_STATS,
      PLAYER_VISUAL_CONFIG,
      RESIST_DATA,
      ATTACK_DATA,
      MOVEMENT_DATA,
      IMAGE_DATA,
      UNKNOWN_F8F2_DATA,
    };

    Type type;
    size_t count;
    DataType data_type;
    const char* name;

    Argument(Type type, size_t count = 0, const char* name = nullptr)
        : type(type),
          count(count),
          data_type(DataType::NONE),
          name(name) {}
    Argument(Type type, DataType data_type, const char* name = nullptr)
        : type(type),
          count(0),
          data_type(data_type),
          name(name) {}
  };

  uint16_t opcode;
  const char* name;
  std::vector<Argument> imm_args;
  std::vector<Argument> stack_args;
  uint16_t version_flags;
  bool preserve_args_list;

  QuestScriptOpcodeDefinition(
      uint16_t opcode,
      const char* name,
      std::vector<Argument> imm_args,
      std::vector<Argument> stack_args,
      uint16_t version_flags,
      bool preserve_args_list = false)
      : opcode(opcode),
        name(name),
        imm_args(imm_args),
        stack_args(stack_args),
        version_flags(version_flags),
        preserve_args_list(preserve_args_list) {}
};

constexpr uint16_t v_flag(QuestScriptVersion v) {
  return (1 << static_cast<uint16_t>(v));
}

using Arg = QuestScriptOpcodeDefinition::Argument;
using V = QuestScriptVersion;

static constexpr uint16_t F_DC_NTE = v_flag(V::DC_NTE);
static constexpr uint16_t F_DC_V1 = v_flag(V::DC_V1);
static constexpr uint16_t F_DC_V2 = v_flag(V::DC_V2);
static constexpr uint16_t F_PC_V2 = v_flag(V::PC_V2);
static constexpr uint16_t F_GC_NTE = v_flag(V::GC_NTE);
static constexpr uint16_t F_GC_V3 = v_flag(V::GC_V3);
static constexpr uint16_t F_XB_V3 = v_flag(V::XB_V3);
static constexpr uint16_t F_GC_EP3 = v_flag(V::GC_EP3);
static constexpr uint16_t F_BB_V4 = v_flag(V::BB_V4);

// clang-format off
static constexpr uint16_t F_V0_V4 = F_DC_NTE | F_DC_V1 | F_DC_V2 | F_PC_V2 | F_GC_NTE | F_GC_V3 | F_XB_V3 | F_GC_EP3 | F_BB_V4;
static constexpr uint16_t F_V0_V2 = F_DC_NTE | F_DC_V1 | F_DC_V2 | F_PC_V2 | F_GC_NTE;
static constexpr uint16_t F_V1_V2 =            F_DC_V1 | F_DC_V2 | F_PC_V2 | F_GC_NTE;
static constexpr uint16_t F_V1_V4 =            F_DC_V1 | F_DC_V2 | F_PC_V2 | F_GC_NTE | F_GC_V3 | F_XB_V3 | F_GC_EP3 | F_BB_V4;
static constexpr uint16_t F_V2    =                      F_DC_V2 | F_PC_V2 | F_GC_NTE;
static constexpr uint16_t F_V2_V4 =                      F_DC_V2 | F_PC_V2 | F_GC_NTE | F_GC_V3 | F_XB_V3 | F_GC_EP3 | F_BB_V4;
static constexpr uint16_t F_V3    =                                                     F_GC_V3 | F_XB_V3 | F_GC_EP3;
static constexpr uint16_t F_V3_V4 =                                                     F_GC_V3 | F_XB_V3 | F_GC_EP3 | F_BB_V4;
static constexpr uint16_t F_V4    =                                                                                    F_BB_V4;
// clang-format on

static constexpr auto LABEL16 = Arg::Type::LABEL16;
static constexpr auto LABEL16_SET = Arg::Type::LABEL16_SET;
static constexpr auto LABEL32 = Arg::Type::LABEL32;
static constexpr auto REG = Arg::Type::REG;
static constexpr auto REG_SET = Arg::Type::REG_SET;
static constexpr auto REG_SET_FIXED = Arg::Type::REG_SET_FIXED;
static constexpr auto REG32 = Arg::Type::REG32;
static constexpr auto REG32_SET_FIXED = Arg::Type::REG32_SET_FIXED;
static constexpr auto INT8 = Arg::Type::INT8;
static constexpr auto INT16 = Arg::Type::INT16;
static constexpr auto INT32 = Arg::Type::INT32;
static constexpr auto FLOAT32 = Arg::Type::FLOAT32;
static constexpr auto CSTRING = Arg::Type::CSTRING;

static const Arg SCRIPT16(LABEL16, Arg::DataType::SCRIPT);
static const Arg SCRIPT16_SET(LABEL16_SET, Arg::DataType::SCRIPT);
static const Arg SCRIPT32(LABEL32, Arg::DataType::SCRIPT);
static const Arg DATA16(LABEL16, Arg::DataType::DATA);
static const Arg CSTRING_LABEL16(LABEL16, Arg::DataType::CSTRING);

static const Arg CLIENT_ID(INT32, 0, "client_id");
static const Arg ITEM_ID(INT32, 0, "item_id");
static const Arg AREA(INT32, 0, "area");

static const QuestScriptOpcodeDefinition opcode_defs[] = {
    {0x0000, "nop", {}, {}, F_V0_V4}, // Does nothing
    {0x0001, "ret", {}, {}, F_V0_V4}, // Pops new PC off stack
    {0x0002, "sync", {}, {}, F_V0_V4}, // Stops execution for the current frame
    {0x0003, "exit", {INT32}, {}, F_V0_V4}, // Exits entirely
    {0x0004, "thread", {SCRIPT16}, {}, F_V0_V4}, // Starts a new thread
    {0x0005, "va_start", {}, {}, F_V3_V4}, // Pushes r1-r7 to the stack
    {0x0006, "va_end", {}, {}, F_V3_V4}, // Pops r7-r1 from the stack
    {0x0007, "va_call", {SCRIPT16}, {}, F_V3_V4}, // Replaces r1-r7 with the args stack, then calls the function
    {0x0008, "let", {REG, REG}, {}, F_V0_V4}, // Copies a value from regB to regA
    {0x0009, "leti", {REG, INT32}, {}, F_V0_V4}, // Sets register to a fixed value (int32)
    {0x000A, "leta", {REG, REG}, {}, F_V0_V2}, // Sets regA to the memory address of regB
    {0x000A, "letb", {REG, INT8}, {}, F_V3_V4}, // Sets register to a fixed value (int8)
    {0x000B, "letw", {REG, INT16}, {}, F_V3_V4}, // Sets register to a fixed value (int16)
    {0x000C, "leta", {REG, REG}, {}, F_V3_V4}, // Sets regA to the memory address of regB
    {0x000D, "leto", {REG, SCRIPT16}, {}, F_V3_V4}, // Sets register to the offset (NOT memory address) of a function
    {0x0010, "set", {REG}, {}, F_V0_V4}, // Sets a register to 1
    {0x0011, "clear", {REG}, {}, F_V0_V4}, // Sets a register to 0
    {0x0012, "rev", {REG}, {}, F_V0_V4}, // Sets a register to 0 if it's nonzero and vice versa
    {0x0013, "gset", {INT16}, {}, F_V0_V4}, // Sets a global flag
    {0x0014, "gclear", {INT16}, {}, F_V0_V4}, // Clears a global flag
    {0x0015, "grev", {INT16}, {}, F_V0_V4}, // Flips a global flag
    {0x0016, "glet", {INT16, REG}, {}, F_V0_V4}, // Sets a global flag to a specific value
    {0x0017, "gget", {INT16, REG}, {}, F_V0_V4}, // Gets a global flag
    {0x0018, "add", {REG, REG}, {}, F_V0_V4}, // regA += regB
    {0x0019, "addi", {REG, INT32}, {}, F_V0_V4}, // regA += imm
    {0x001A, "sub", {REG, REG}, {}, F_V0_V4}, // regA -= regB
    {0x001B, "subi", {REG, INT32}, {}, F_V0_V4}, // regA -= imm
    {0x001C, "mul", {REG, REG}, {}, F_V0_V4}, // regA *= regB
    {0x001D, "muli", {REG, INT32}, {}, F_V0_V4}, // regA *= imm
    {0x001E, "div", {REG, REG}, {}, F_V0_V4}, // regA /= regB
    {0x001F, "divi", {REG, INT32}, {}, F_V0_V4}, // regA /= imm
    {0x0020, "and", {REG, REG}, {}, F_V0_V4}, // regA &= regB
    {0x0021, "andi", {REG, INT32}, {}, F_V0_V4}, // regA &= imm
    {0x0022, "or", {REG, REG}, {}, F_V0_V4}, // regA |= regB
    {0x0023, "ori", {REG, INT32}, {}, F_V0_V4}, // regA |= imm
    {0x0024, "xor", {REG, REG}, {}, F_V0_V4}, // regA ^= regB
    {0x0025, "xori", {REG, INT32}, {}, F_V0_V4}, // regA ^= imm
    {0x0026, "mod", {REG, REG}, {}, F_V3_V4}, // regA %= regB
    {0x0027, "modi", {REG, INT32}, {}, F_V3_V4}, // regA %= imm
    {0x0028, "jmp", {SCRIPT16}, {}, F_V0_V4}, // Jumps to function_table[fn_id]
    {0x0029, "call", {SCRIPT16}, {}, F_V0_V4}, // Pushes the offset after this opcode and jumps to function_table[fn_id]
    {0x002A, "jmp_on", {SCRIPT16, REG_SET}, {}, F_V0_V4}, // If all given registers are nonzero, jumps to function_table[fn_id]
    {0x002B, "jmp_off", {SCRIPT16, REG_SET}, {}, F_V0_V4}, // If all given registers are zero, jumps to function_table[fn_id]
    {0x002C, "jmp_eq", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA == regB, jumps to function_table[fn_id]
    {0x002D, "jmpi_eq", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA == regB, jumps to function_table[fn_id]
    {0x002E, "jmp_ne", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA != regB, jumps to function_table[fn_id]
    {0x002F, "jmpi_ne", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA != regB, jumps to function_table[fn_id]
    {0x0030, "ujmp_gt", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0031, "ujmpi_gt", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0032, "jmp_gt", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0033, "jmpi_gt", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0034, "ujmp_lt", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0035, "ujmpi_lt", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0036, "jmp_lt", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0037, "jmpi_lt", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0038, "ujmp_ge", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x0039, "ujmpi_ge", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x003A, "jmp_ge", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x003B, "jmpi_ge", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x003C, "ujmp_le", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x003D, "ujmpi_le", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x003E, "jmp_le", {REG, REG, SCRIPT16}, {}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x003F, "jmpi_le", {REG, INT32, SCRIPT16}, {}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x0040, "switch_jmp", {REG, SCRIPT16_SET}, {}, F_V0_V4}, // Jumps to function_table[fn_ids[regA]]
    {0x0041, "switch_call", {REG, SCRIPT16_SET}, {}, F_V0_V4}, // Calls function_table[fn_ids[regA]]
    {0x0042, "nop_42", {INT32}, {}, F_V0_V2}, // Does nothing
    {0x0042, "stack_push", {REG}, {}, F_V3_V4}, // Pushes regA
    {0x0043, "stack_pop", {REG}, {}, F_V3_V4}, // Pops regA
    {0x0044, "stack_pushm", {REG, INT32}, {}, F_V3_V4}, // Pushes N regs in increasing order starting at regA
    {0x0045, "stack_popm", {REG, INT32}, {}, F_V3_V4}, // Pops N regs in decreasing order ending at regA
    {0x0048, "arg_pushr", {REG}, {}, F_V3_V4, true}, // Pushes regA to the args list
    {0x0049, "arg_pushl", {INT32}, {}, F_V3_V4, true}, // Pushes imm to the args list
    {0x004A, "arg_pushb", {INT8}, {}, F_V3_V4, true}, // Pushes imm to the args list
    {0x004B, "arg_pushw", {INT16}, {}, F_V3_V4, true}, // Pushes imm to the args list
    {0x004C, "arg_pusha", {REG}, {}, F_V3_V4, true}, // Pushes memory address of regA to the args list
    {0x004D, "arg_pusho", {LABEL16}, {}, F_V3_V4, true}, // Pushes function_table[fn_id] to the args list
    {0x004E, "arg_pushs", {CSTRING}, {}, F_V3_V4, true}, // Pushes memory address of str to the args list
    {0x0050, "message", {INT32, CSTRING}, {}, F_V0_V2}, // Creates a dialogue with object/NPC N starting with message str
    {0x0050, "message", {}, {INT32, CSTRING}, F_V3_V4}, // Creates a dialogue with object/NPC N starting with message str
    {0x0051, "list", {REG, CSTRING}, {}, F_V0_V2}, // Prompts the player with a list of choices, returning the index of their choice in regA
    {0x0051, "list", {}, {REG, CSTRING}, F_V3_V4}, // Prompts the player with a list of choices, returning the index of their choice in regA
    {0x0052, "fadein", {}, {}, F_V0_V4}, // Fades from black
    {0x0053, "fadeout", {}, {}, F_V0_V4}, // Fades to black
    {0x0054, "se", {INT32}, {}, F_V0_V2}, // Plays a sound effect
    {0x0054, "se", {}, {INT32}, F_V3_V4}, // Plays a sound effect
    {0x0055, "bgm", {INT32}, {}, F_V0_V2}, // Plays a fanfare (clear.adx or miniclear.adx)
    {0x0055, "bgm", {}, {INT32}, F_V3_V4}, // Plays a fanfare (clear.adx or miniclear.adx)
    {0x0056, "nop_56", {}, {}, F_V0_V2}, // Does nothing
    {0x0057, "nop_57", {}, {}, F_V0_V2}, // Does nothing
    {0x0058, "nop_58", {INT32}, {}, F_V0_V2}, // Does nothing
    {0x0059, "nop_59", {INT32}, {}, F_V0_V2}, // Does nothing
    {0x005A, "window_msg", {CSTRING}, {}, F_V0_V2}, // Displays a message
    {0x005A, "window_msg", {}, {CSTRING}, F_V3_V4}, // Displays a message
    {0x005B, "add_msg", {CSTRING}, {}, F_V0_V2}, // Adds a message to an existing window
    {0x005B, "add_msg", {}, {CSTRING}, F_V3_V4}, // Adds a message to an existing window
    {0x005C, "mesend", {}, {}, F_V0_V4}, // Closes a message box
    {0x005D, "gettime", {REG}, {}, F_V0_V4}, // Gets the current time
    {0x005E, "winend", {}, {}, F_V0_V4}, // Closes a window_msg
    {0x0060, "npc_crt", {INT32, INT32}, {}, F_V0_V2}, // Creates an NPC
    {0x0060, "npc_crt", {}, {INT32, INT32}, F_V3_V4}, // Creates an NPC
    {0x0061, "npc_stop", {INT32}, {}, F_V0_V2}, // Tells an NPC to stop following
    {0x0061, "npc_stop", {}, {INT32}, F_V3_V4}, // Tells an NPC to stop following
    {0x0062, "npc_play", {INT32}, {}, F_V0_V2}, // Tells an NPC to follow the player
    {0x0062, "npc_play", {}, {INT32}, F_V3_V4}, // Tells an NPC to follow the player
    {0x0063, "npc_kill", {INT32}, {}, F_V0_V2}, // Destroys an NPC
    {0x0063, "npc_kill", {}, {INT32}, F_V3_V4}, // Destroys an NPC
    {0x0064, "npc_nont", {}, {}, F_V0_V4},
    {0x0065, "npc_talk", {}, {}, F_V0_V4},
    {0x0066, "npc_crp", {{REG_SET_FIXED, 6}, INT32}, {}, F_V0_V2}, // Creates an NPC. Second argument is ignored
    {0x0066, "npc_crp", {{REG_SET_FIXED, 6}}, {}, F_V3_V4}, // Creates an NPC
    {0x0068, "create_pipe", {INT32}, {}, F_V0_V2}, // Creates a pipe
    {0x0068, "create_pipe", {}, {INT32}, F_V3_V4}, // Creates a pipe
    {0x0069, "p_hpstat", {REG, CLIENT_ID}, {}, F_V0_V2}, // Compares player HP with a given value
    {0x0069, "p_hpstat", {}, {REG, CLIENT_ID}, F_V3_V4}, // Compares player HP with a given value
    {0x006A, "p_dead", {REG, CLIENT_ID}, {}, F_V0_V2}, // Checks if player is dead
    {0x006A, "p_dead", {}, {REG, CLIENT_ID}, F_V3_V4}, // Checks if player is dead
    {0x006B, "p_disablewarp", {}, {}, F_V0_V4}, // Disables telepipes/Ryuker
    {0x006C, "p_enablewarp", {}, {}, F_V0_V4}, // Enables telepipes/Ryuker
    {0x006D, "p_move", {{REG_SET_FIXED, 5}, INT32}, {}, F_V0_V2}, // Moves player. Second argument is ignored
    {0x006D, "p_move", {{REG_SET_FIXED, 5}}, {}, F_V3_V4}, // Moves player
    {0x006E, "p_look", {CLIENT_ID}, {}, F_V0_V2},
    {0x006E, "p_look", {}, {CLIENT_ID}, F_V3_V4},
    {0x0070, "p_action_disable", {}, {}, F_V0_V4}, // Disables attacks for all players
    {0x0071, "p_action_enable", {}, {}, F_V0_V4}, // Enables attacks for all players
    {0x0072, "disable_movement1", {CLIENT_ID}, {}, F_V0_V2}, // Disables movement for the given player
    {0x0072, "disable_movement1", {}, {CLIENT_ID}, F_V3_V4}, // Disables movement for the given player
    {0x0073, "enable_movement1", {CLIENT_ID}, {}, F_V0_V2}, // Enables movement for the given player
    {0x0073, "enable_movement1", {}, {CLIENT_ID}, F_V3_V4}, // Enables movement for the given player
    {0x0074, "p_noncol", {}, {}, F_V0_V4},
    {0x0075, "p_col", {}, {}, F_V0_V4},
    {0x0076, "p_setpos", {CLIENT_ID, {REG_SET_FIXED, 4}}, {}, F_V0_V2},
    {0x0076, "p_setpos", {}, {CLIENT_ID, {REG_SET_FIXED, 4}}, F_V3_V4},
    {0x0077, "p_return_guild", {}, {}, F_V0_V4},
    {0x0078, "p_talk_guild", {CLIENT_ID}, {}, F_V0_V2},
    {0x0078, "p_talk_guild", {}, {CLIENT_ID}, F_V3_V4},
    {0x0079, "npc_talk_pl", {{REG32_SET_FIXED, 8}}, {}, F_V0_V2},
    {0x0079, "npc_talk_pl", {{REG_SET_FIXED, 8}}, {}, F_V3_V4},
    {0x007A, "npc_talk_kill", {INT32}, {}, F_V0_V2},
    {0x007A, "npc_talk_kill", {}, {INT32}, F_V3_V4},
    {0x007B, "npc_crtpk", {INT32, INT32}, {}, F_V0_V2}, // Creates attacker NPC
    {0x007B, "npc_crtpk", {}, {INT32, INT32}, F_V3_V4}, // Creates attacker NPC
    {0x007C, "npc_crppk", {{REG32_SET_FIXED, 7}, INT32}, {}, F_V0_V2}, // Creates attacker NPC
    {0x007C, "npc_crppk", {{REG_SET_FIXED, 7}}, {}, F_V3_V4}, // Creates attacker NPC
    {0x007D, "npc_crptalk", {{REG32_SET_FIXED, 6}, INT32}, {}, F_V0_V2},
    {0x007D, "npc_crptalk", {{REG_SET_FIXED, 6}}, {}, F_V3_V4},
    {0x007E, "p_look_at", {CLIENT_ID, CLIENT_ID}, {}, F_V0_V2},
    {0x007E, "p_look_at", {}, {CLIENT_ID, CLIENT_ID}, F_V3_V4},
    {0x007F, "npc_crp_id", {{REG32_SET_FIXED, 7}, INT32}, {}, F_V0_V2},
    {0x007F, "npc_crp_id", {{REG_SET_FIXED, 7}}, {}, F_V3_V4},
    {0x0080, "cam_quake", {}, {}, F_V0_V4},
    {0x0081, "cam_adj", {}, {}, F_V0_V4},
    {0x0082, "cam_zmin", {}, {}, F_V0_V4},
    {0x0083, "cam_zmout", {}, {}, F_V0_V4},
    {0x0084, "cam_pan", {{REG32_SET_FIXED, 5}, INT32}, {}, F_V0_V2},
    {0x0084, "cam_pan", {{REG_SET_FIXED, 5}}, {}, F_V3_V4},
    {0x0085, "game_lev_super", {}, {}, F_V0_V2},
    {0x0085, "nop_85", {}, {}, F_V3_V4},
    {0x0086, "game_lev_reset", {}, {}, F_V0_V2},
    {0x0086, "nop_86", {}, {}, F_V3_V4},
    {0x0087, "pos_pipe", {{REG32_SET_FIXED, 4}, INT32}, {}, F_V0_V2},
    {0x0087, "pos_pipe", {{REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0x0088, "if_zone_clear", {REG, {REG_SET_FIXED, 2}}, {}, F_V0_V4},
    {0x0089, "chk_ene_num", {REG}, {}, F_V0_V4},
    {0x008A, "unhide_obj", {{REG_SET_FIXED, 3}}, {}, F_V0_V4},
    {0x008B, "unhide_ene", {{REG_SET_FIXED, 3}}, {}, F_V0_V4},
    {0x008C, "at_coords_call", {{REG_SET_FIXED, 5}}, {}, F_V0_V4},
    {0x008D, "at_coords_talk", {{REG_SET_FIXED, 5}}, {}, F_V0_V4},
    {0x008E, "col_npcin", {{REG_SET_FIXED, 5}}, {}, F_V0_V4},
    {0x008F, "col_npcinr", {{REG_SET_FIXED, 6}}, {}, F_V0_V4},
    {0x0090, "switch_on", {INT32}, {}, F_V0_V2},
    {0x0090, "switch_on", {}, {INT32}, F_V3_V4},
    {0x0091, "switch_off", {INT32}, {}, F_V0_V2},
    {0x0091, "switch_off", {}, {INT32}, F_V3_V4},
    {0x0092, "playbgm_epi", {INT32}, {}, F_V0_V2},
    {0x0092, "playbgm_epi", {}, {INT32}, F_V3_V4},
    {0x0093, "set_mainwarp", {INT32}, {}, F_V0_V2},
    {0x0093, "set_mainwarp", {}, {INT32}, F_V3_V4},
    {0x0094, "set_obj_param", {{REG_SET_FIXED, 6}, REG}, {}, F_V0_V4},
    {0x0095, "set_floor_handler", {AREA, SCRIPT32}, {}, F_V0_V2},
    {0x0095, "set_floor_handler", {}, {AREA, SCRIPT16}, F_V3_V4},
    {0x0096, "clr_floor_handler", {AREA}, {}, F_V0_V2},
    {0x0096, "clr_floor_handler", {}, {AREA}, F_V3_V4},
    {0x0097, "col_plinaw", {{REG_SET_FIXED, 9}}, {}, F_V1_V4},
    {0x0098, "hud_hide", {}, {}, F_V0_V4},
    {0x0099, "hud_show", {}, {}, F_V0_V4},
    {0x009A, "cine_enable", {}, {}, F_V0_V4},
    {0x009B, "cine_disable", {}, {}, F_V0_V4},
    {0x00A0, "nop_A0_debug", {INT32, CSTRING}, {}, F_V0_V2}, // argA appears unused; game will softlock unless argB contains exactly 2 messages
    {0x00A0, "nop_A0_debug", {}, {INT32, CSTRING}, F_V3_V4},
    {0x00A1, "set_qt_failure", {SCRIPT32}, {}, F_V0_V2},
    {0x00A1, "set_qt_failure", {SCRIPT16}, {}, F_V3_V4},
    {0x00A2, "set_qt_success", {SCRIPT32}, {}, F_V0_V2},
    {0x00A2, "set_qt_success", {SCRIPT16}, {}, F_V3_V4},
    {0x00A3, "clr_qt_failure", {}, {}, F_V0_V4},
    {0x00A4, "clr_qt_success", {}, {}, F_V0_V4},
    {0x00A5, "set_qt_cancel", {SCRIPT32}, {}, F_V0_V2},
    {0x00A5, "set_qt_cancel", {SCRIPT16}, {}, F_V3_V4},
    {0x00A6, "clr_qt_cancel", {}, {}, F_V0_V4},
    {0x00A8, "pl_walk", {{REG32_SET_FIXED, 4}, INT32}, {}, F_V0_V2},
    {0x00A8, "pl_walk", {{REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0x00B0, "pl_add_meseta", {CLIENT_ID, INT32}, {}, F_V0_V2},
    {0x00B0, "pl_add_meseta", {}, {CLIENT_ID, INT32}, F_V3_V4},
    {0x00B1, "thread_stg", {SCRIPT16}, {}, F_V0_V4},
    {0x00B2, "del_obj_param", {REG}, {}, F_V0_V4},
    {0x00B3, "item_create", {{REG_SET_FIXED, 3}, REG}, {}, F_V0_V4}, // Creates an item; regsA holds item data1[0-2], regB receives item ID
    {0x00B4, "item_create2", {{REG_SET_FIXED, 12}, REG}, {}, F_V0_V4}, // Like item_create but input regs each specify 1 byte (and can specify all of data1)
    {0x00B5, "item_delete", {REG, {REG_SET_FIXED, 12}}, {}, F_V0_V4},
    {0x00B6, "item_delete2", {{REG_SET_FIXED, 3}, {REG_SET_FIXED, 12}}, {}, F_V0_V4},
    {0x00B7, "item_check", {{REG_SET_FIXED, 3}, REG}, {}, F_V0_V4},
    {0x00B8, "setevt", {INT32}, {}, F_V1_V2},
    {0x00B8, "setevt", {}, {INT32}, F_V3_V4},
    {0x00B9, "get_difficulty_level_v1", {REG}, {}, F_V1_V4}, // Only returns 0-2, even in Ultimate (which results in 2 as well). Presumably all non-v1 quests should use get_difficulty_level_v2 instead.
    {0x00BA, "set_qt_exit", {SCRIPT32}, {}, F_V1_V2},
    {0x00BA, "set_qt_exit", {SCRIPT16}, {}, F_V3_V4},
    {0x00BB, "clr_qt_exit", {}, {}, F_V1_V4},
    {0x00BC, "nop_BC", {CSTRING}, {}, F_V1_V4},
    {0x00C0, "particle", {{REG32_SET_FIXED, 5}, INT32}, {}, F_V1_V2},
    {0x00C0, "particle", {{REG_SET_FIXED, 5}}, {}, F_V3_V4},
    {0x00C1, "npc_text", {INT32, CSTRING}, {}, F_V1_V2},
    {0x00C1, "npc_text", {}, {INT32, CSTRING}, F_V3_V4},
    {0x00C2, "npc_chkwarp", {}, {}, F_V1_V4},
    {0x00C3, "pl_pkoff", {}, {}, F_V1_V4},
    {0x00C4, "map_designate", {{REG_SET_FIXED, 4}}, {}, F_V1_V4},
    {0x00C5, "masterkey_on", {}, {}, F_V1_V4},
    {0x00C6, "masterkey_off", {}, {}, F_V1_V4},
    {0x00C7, "window_time", {}, {}, F_V1_V4},
    {0x00C8, "winend_time", {}, {}, F_V1_V4},
    {0x00C9, "winset_time", {REG}, {}, F_V1_V4},
    {0x00CA, "getmtime", {REG}, {}, F_V1_V4},
    {0x00CB, "set_quest_board_handler", {INT32, SCRIPT32, CSTRING}, {}, F_V1_V2},
    {0x00CB, "set_quest_board_handler", {}, {INT32, SCRIPT16, CSTRING}, F_V3_V4},
    {0x00CC, "clear_quest_board_handler", {INT32}, {}, F_V1_V2},
    {0x00CC, "clear_quest_board_handler", {}, {INT32}, F_V3_V4},
    {0x00CD, "particle_id", {{REG32_SET_FIXED, 4}, INT32}, {}, F_V1_V2},
    {0x00CD, "particle_id", {{REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0x00CE, "npc_crptalk_id", {{REG32_SET_FIXED, 7}, INT32}, {}, F_V1_V2},
    {0x00CE, "npc_crptalk_id", {{REG_SET_FIXED, 7}}, {}, F_V3_V4},
    {0x00CF, "npc_lang_clean", {}, {}, F_V1_V4},
    {0x00D0, "pl_pkon", {}, {}, F_V1_V4},
    {0x00D1, "pl_chk_item2", {{REG_SET_FIXED, 4}, REG}, {}, F_V1_V4}, // Presumably like item_check but also checks data2
    {0x00D2, "enable_mainmenu", {}, {}, F_V1_V4},
    {0x00D3, "disable_mainmenu", {}, {}, F_V1_V4},
    {0x00D4, "start_battlebgm", {}, {}, F_V1_V4},
    {0x00D5, "end_battlebgm", {}, {}, F_V1_V4},
    {0x00D6, "disp_msg_qb", {CSTRING}, {}, F_V1_V2},
    {0x00D6, "disp_msg_qb", {}, {CSTRING}, F_V3_V4},
    {0x00D7, "close_msg_qb", {}, {}, F_V1_V4},
    {0x00D8, "set_eventflag", {INT32, INT32}, {}, F_V1_V2},
    {0x00D8, "set_eventflag", {}, {INT32, INT32}, F_V3_V4},
    {0x00D9, "sync_register", {INT32, INT32}, {}, F_V1_V2},
    {0x00D9, "sync_register", {}, {INT32, INT32}, F_V3_V4},
    {0x00DA, "set_returnhunter", {}, {}, F_V1_V4},
    {0x00DB, "set_returncity", {}, {}, F_V1_V4},
    {0x00DC, "load_pvr", {}, {}, F_V1_V4},
    {0x00DD, "load_midi", {}, {}, F_V1_V4}, // Seems incomplete on V3 and BB - has some similar codepaths as load_pvr, but the function that actually process the data seems to do nothing
    {0x00DE, "item_detect_bank", {{REG_SET_FIXED, 6}, REG}, {}, F_V1_V4}, // regsA specifies the first 6 bytes of an ItemData (data1[0-5])
    {0x00DF, "npc_param", {{REG32_SET_FIXED, 14}, INT32}, {}, F_V1_V2},
    {0x00DF, "npc_param", {{REG_SET_FIXED, 14}, INT32}, {}, F_V3_V4},
    {0x00E0, "pad_dragon", {}, {}, F_V1_V4},
    {0x00E1, "clear_mainwarp", {INT32}, {}, F_V1_V2},
    {0x00E1, "clear_mainwarp", {}, {INT32}, F_V3_V4},
    {0x00E2, "pcam_param", {{REG32_SET_FIXED, 6}}, {}, F_V1_V2},
    {0x00E2, "pcam_param", {{REG_SET_FIXED, 6}}, {}, F_V3_V4},
    {0x00E3, "start_setevt", {INT32, INT32}, {}, F_V1_V2},
    {0x00E3, "start_setevt", {}, {INT32, INT32}, F_V3_V4},
    {0x00E4, "warp_on", {}, {}, F_V1_V4},
    {0x00E5, "warp_off", {}, {}, F_V1_V4},
    {0x00E6, "get_client_id", {REG}, {}, F_V1_V4},
    {0x00E7, "get_leader_id", {REG}, {}, F_V1_V4},
    {0x00E8, "set_eventflag2", {INT32, REG}, {}, F_V1_V2},
    {0x00E8, "set_eventflag2", {}, {INT32, REG}, F_V3_V4},
    {0x00E9, "mod2", {REG, REG}, {}, F_V1_V4},
    {0x00EA, "modi2", {REG, INT32}, {}, F_V1_V4},
    {0x00EB, "enable_bgmctrl", {INT32}, {}, F_V1_V2},
    {0x00EB, "enable_bgmctrl", {}, {INT32}, F_V3_V4},
    {0x00EC, "sw_send", {{REG_SET_FIXED, 3}}, {}, F_V1_V4},
    {0x00ED, "create_bgmctrl", {}, {}, F_V1_V4},
    {0x00EE, "pl_add_meseta2", {INT32}, {}, F_V1_V2},
    {0x00EE, "pl_add_meseta2", {}, {INT32}, F_V3_V4},
    {0x00EF, "sync_register2", {INT32, REG32}, {}, F_V1_V2},
    {0x00EF, "sync_register2", {}, {INT32, INT32}, F_V3_V4},
    {0x00F0, "send_regwork", {INT32, REG32}, {}, F_V1_V2},
    {0x00F1, "leti_fixed_camera", {{REG32_SET_FIXED, 6}}, {}, F_V2},
    {0x00F1, "leti_fixed_camera", {{REG_SET_FIXED, 6}}, {}, F_V3_V4},
    {0x00F2, "default_camera_pos1", {}, {}, F_V2_V4},
    {0xF800, "debug_F800", {}, {}, F_V2}, // Same as 50, but uses fixed arguments - with a Japanese string that Google Translate translates as "I'm frugal!!"
    {0xF801, "set_chat_callback", {{REG32_SET_FIXED, 5}, CSTRING}, {}, F_V2},
    {0xF801, "set_chat_callback", {}, {{REG_SET_FIXED, 5}, CSTRING}, F_V3_V4},
    {0xF808, "get_difficulty_level_v2", {REG}, {}, F_V2_V4},
    {0xF809, "get_number_of_players", {REG}, {}, F_V2_V4},
    {0xF80A, "get_coord_of_player", {{REG_SET_FIXED, 3}, REG}, {}, F_V2_V4},
    {0xF80B, "enable_map", {}, {}, F_V2_V4},
    {0xF80C, "disable_map", {}, {}, F_V2_V4},
    {0xF80D, "map_designate_ex", {{REG_SET_FIXED, 5}}, {}, F_V2_V4},
    {0xF80E, "disable_weapon_drop", {CLIENT_ID}, {}, F_V2},
    {0xF80E, "disable_weapon_drop", {}, {CLIENT_ID}, F_V3_V4},
    {0xF80F, "enable_weapon_drop", {CLIENT_ID}, {}, F_V2},
    {0xF80F, "enable_weapon_drop", {}, {CLIENT_ID}, F_V3_V4},
    {0xF810, "ba_initial_floor", {AREA}, {}, F_V2},
    {0xF810, "ba_initial_floor", {}, {AREA}, F_V3_V4},
    {0xF811, "set_ba_rules", {}, {}, F_V2_V4},
    {0xF812, "ba_set_tech", {INT32}, {}, F_V2},
    {0xF812, "ba_set_tech", {}, {INT32}, F_V3_V4},
    {0xF813, "ba_set_equip", {INT32}, {}, F_V2},
    {0xF813, "ba_set_equip", {}, {INT32}, F_V3_V4},
    {0xF814, "ba_set_mag", {INT32}, {}, F_V2},
    {0xF814, "ba_set_mag", {}, {INT32}, F_V3_V4},
    {0xF815, "ba_set_item", {INT32}, {}, F_V2},
    {0xF815, "ba_set_item", {}, {INT32}, F_V3_V4},
    {0xF816, "ba_set_trapmenu", {INT32}, {}, F_V2},
    {0xF816, "ba_set_trapmenu", {}, {INT32}, F_V3_V4},
    {0xF817, "ba_set_unused_F817", {INT32}, {}, F_V2}, // This appears to be unused - the value is copied into the main battle rules struct, but then the field appears never to be read
    {0xF817, "ba_set_unused_F817", {}, {INT32}, F_V3_V4},
    {0xF818, "ba_set_respawn", {INT32}, {}, F_V2},
    {0xF818, "ba_set_respawn", {}, {INT32}, F_V3_V4},
    {0xF819, "ba_set_char", {INT32}, {}, F_V2},
    {0xF819, "ba_set_char", {}, {INT32}, F_V3_V4},
    {0xF81A, "ba_dropwep", {INT32}, {}, F_V2},
    {0xF81A, "ba_dropwep", {}, {INT32}, F_V3_V4},
    {0xF81B, "ba_teams", {INT32}, {}, F_V2},
    {0xF81B, "ba_teams", {}, {INT32}, F_V3_V4},
    {0xF81C, "ba_disp_msg", {CSTRING}, {}, F_V2},
    {0xF81C, "ba_disp_msg", {}, {CSTRING}, F_V3_V4},
    {0xF81D, "death_lvl_up", {INT32}, {}, F_V2},
    {0xF81D, "death_lvl_up", {}, {INT32}, F_V3_V4},
    {0xF81E, "ba_set_meseta", {INT32}, {}, F_V2},
    {0xF81E, "ba_set_meseta", {}, {INT32}, F_V3_V4},
    {0xF820, "cmode_stage", {INT32}, {}, F_V2},
    {0xF820, "cmode_stage", {}, {INT32}, F_V3_V4},
    {0xF821, "nop_F821", {{REG_SET_FIXED, 9}}, {}, F_V2_V4}, // regsA[3-8] specify first 6 bytes of an ItemData. This opcode consumes an item ID, but does nothing else.
    {0xF822, "nop_F822", {REG}, {}, F_V2_V4},
    {0xF823, "set_cmode_char_template", {INT32}, {}, F_V2},
    {0xF823, "set_cmode_char_template", {}, {INT32}, F_V3_V4},
    {0xF824, "set_cmode_diff", {INT32}, {}, F_V2},
    {0xF824, "set_cmode_diff", {}, {INT32}, F_V3_V4},
    {0xF825, "exp_multiplication", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF826, "if_player_alive_cm", {REG}, {}, F_V2_V4},
    {0xF827, "get_user_is_dead", {REG}, {}, F_V2_V4},
    {0xF828, "go_floor", {REG, REG}, {}, F_V2_V4},
    {0xF829, "get_num_kills", {REG, REG}, {}, F_V2_V4},
    {0xF82A, "reset_kills", {REG}, {}, F_V2_V4},
    {0xF82B, "unlock_door2", {INT32, INT32}, {}, F_V2},
    {0xF82B, "unlock_door2", {}, {INT32, INT32}, F_V3_V4},
    {0xF82C, "lock_door2", {INT32, INT32}, {}, F_V2},
    {0xF82C, "lock_door2", {}, {INT32, INT32}, F_V3_V4},
    {0xF82D, "if_switch_not_pressed", {{REG_SET_FIXED, 2}}, {}, F_V2_V4},
    {0xF82E, "if_switch_pressed", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF830, "control_dragon", {REG}, {}, F_V2_V4},
    {0xF831, "release_dragon", {}, {}, F_V2_V4},
    {0xF838, "shrink", {REG}, {}, F_V2_V4},
    {0xF839, "unshrink", {REG}, {}, F_V2_V4},
    {0xF83A, "set_shrink_cam1", {{REG_SET_FIXED, 4}}, {}, F_V2_V4},
    {0xF83B, "set_shrink_cam2", {{REG_SET_FIXED, 4}}, {}, F_V2_V4},
    {0xF83C, "display_clock2", {REG}, {}, F_V2_V4},
    {0xF83D, "set_area_total", {INT32}, {}, F_V2},
    {0xF83D, "set_area_total", {}, {INT32}, F_V3_V4},
    {0xF83E, "delete_area_title", {INT32}, {}, F_V2},
    {0xF83E, "delete_area_title", {}, {INT32}, F_V3_V4},
    {0xF840, "load_npc_data", {}, {}, F_V2_V4},
    {0xF841, "get_npc_data", {{LABEL16, Arg::DataType::PLAYER_VISUAL_CONFIG, "visual_config"}}, {}, F_V2_V4},
    {0xF848, "give_damage_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF849, "take_damage_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF84A, "enemy_give_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4}, // Actual value used is regsA[0] + (regsA[1] / regsA[2])
    {0xF84B, "enemy_take_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4}, // Actual value used is regsA[0] + (regsA[1] / regsA[2])
    {0xF84C, "kill_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF84D, "death_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF84E, "enemy_kill_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4}, // Actual value used is regsA[0] + (regsA[1] / regsA[2])
    {0xF84F, "enemy_death_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF850, "meseta_score", {{REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF851, "ba_set_trap_count", {{REG_SET_FIXED, 2}}, {}, F_V2_V4}, // regsA is [trap_type, trap_count]
    {0xF852, "ba_set_target", {INT32}, {}, F_V2},
    {0xF852, "ba_set_target", {}, {INT32}, F_V3_V4},
    {0xF853, "reverse_warps", {}, {}, F_V2_V4},
    {0xF854, "unreverse_warps", {}, {}, F_V2_V4},
    {0xF855, "set_ult_map", {}, {}, F_V2_V4},
    {0xF856, "unset_ult_map", {}, {}, F_V2_V4},
    {0xF857, "set_area_title", {CSTRING}, {}, F_V2},
    {0xF857, "set_area_title", {}, {CSTRING}, F_V3_V4},
    {0xF858, "ba_show_self_traps", {}, {}, F_V2_V4},
    {0xF859, "ba_hide_self_traps", {}, {}, F_V2_V4},
    {0xF85A, "equip_item", {{REG32_SET_FIXED, 4}}, {}, F_V2}, // regsA are {client_id, item.data1[0-2]}
    {0xF85A, "equip_item", {{REG_SET_FIXED, 4}}, {}, F_V3_V4}, // regsA are {client_id, item.data1[0-2]}
    {0xF85B, "unequip_item", {CLIENT_ID, INT32}, {}, F_V2},
    {0xF85B, "unequip_item", {}, {CLIENT_ID, INT32}, F_V3_V4},
    {0xF85C, "qexit2", {INT32}, {}, F_V2_V4},
    {0xF85D, "set_allow_item_flags", {INT32}, {}, F_V2}, // Same as on v3
    {0xF85D, "set_allow_item_flags", {}, {INT32}, F_V3_V4}, // 0 = allow normal item usage (undoes all of the following), 1 = disallow weapons, 2 = disallow armors, 3 = disallow shields, 4 = disallow units, 5 = disallow mags, 6 = disallow tools
    {0xF85E, "ba_enable_sonar", {INT32}, {}, F_V2},
    {0xF85E, "ba_enable_sonar", {}, {INT32}, F_V3_V4},
    {0xF85F, "ba_use_sonar", {INT32}, {}, F_V2},
    {0xF85F, "ba_use_sonar", {}, {INT32}, F_V3_V4},
    {0xF860, "clear_score_announce", {}, {}, F_V2_V4},
    {0xF861, "set_score_announce", {INT32}, {}, F_V2},
    {0xF861, "set_score_announce", {}, {INT32}, F_V3_V4},
    {0xF862, "give_s_rank_weapon", {REG32, REG32, CSTRING}, {}, F_V2},
    {0xF862, "give_s_rank_weapon", {}, {INT32, REG, CSTRING}, F_V3_V4},
    {0xF863, "get_mag_levels", {{REG32_SET_FIXED, 4}}, {}, F_V2},
    {0xF863, "get_mag_levels", {{REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0xF864, "cmode_rank", {INT32, CSTRING}, {}, F_V2},
    {0xF864, "cmode_rank", {}, {INT32, CSTRING}, F_V3_V4},
    {0xF865, "award_item_name", {}, {}, F_V2_V4},
    {0xF866, "award_item_select", {}, {}, F_V2_V4},
    {0xF867, "award_item_give_to", {REG}, {}, F_V2_V4},
    {0xF868, "set_cmode_rank", {REG, REG}, {}, F_V2_V4},
    {0xF869, "check_rank_time", {REG, REG}, {}, F_V2_V4},
    {0xF86A, "item_create_cmode", {{REG_SET_FIXED, 6}, REG}, {}, F_V2_V4}, // regsA specifies item.data1[0-5]
    {0xF86B, "ba_box_drops", {REG}, {}, F_V2_V4}, // TODO: This sets override_area in TItemDropSub; use this in ItemCreator
    {0xF86C, "award_item_ok", {REG}, {}, F_V2_V4},
    {0xF86D, "ba_set_trapself", {}, {}, F_V2_V4},
    {0xF86E, "ba_clear_trapself", {}, {}, F_V2_V4},
    {0xF86F, "ba_set_lives", {INT32}, {}, F_V2},
    {0xF86F, "ba_set_lives", {}, {INT32}, F_V3_V4},
    {0xF870, "ba_set_tech_lvl", {INT32}, {}, F_V2},
    {0xF870, "ba_set_tech_lvl", {}, {INT32}, F_V3_V4},
    {0xF871, "ba_set_lvl", {INT32}, {}, F_V2},
    {0xF871, "ba_set_lvl", {}, {INT32}, F_V3_V4},
    {0xF872, "ba_set_time_limit", {INT32}, {}, F_V2},
    {0xF872, "ba_set_time_limit", {}, {INT32}, F_V3_V4},
    {0xF873, "dark_falz_is_dead", {REG}, {}, F_V2_V4},
    {0xF874, "set_cmode_rank_override", {INT32, CSTRING}, {}, F_V2}, // argA is an XRGB8888 color, argB is two strings separated by \t or \n: the rank text to check for, and the rank text that should replace it if found
    {0xF874, "set_cmode_rank_override", {}, {INT32, CSTRING}, F_V3_V4},
    {0xF875, "enable_stealth_suit_effect", {REG}, {}, F_V2_V4},
    {0xF876, "disable_stealth_suit_effect", {REG}, {}, F_V2_V4},
    {0xF877, "enable_techs", {REG}, {}, F_V2_V4},
    {0xF878, "disable_techs", {REG}, {}, F_V2_V4},
    {0xF879, "get_gender", {REG, REG}, {}, F_V2_V4},
    {0xF87A, "get_chara_class", {REG, {REG_SET_FIXED, 2}}, {}, F_V2_V4},
    {0xF87B, "take_slot_meseta", {{REG_SET_FIXED, 2}, REG}, {}, F_V2_V4},
    {0xF87C, "get_guild_card_file_creation_time", {REG}, {}, F_V2_V4},
    {0xF87D, "kill_player", {REG}, {}, F_V2_V4},
    {0xF87E, "get_serial_number", {REG}, {}, F_V2_V4}, // Returns 0 on BB
    {0xF87F, "get_eventflag", {REG, REG}, {}, F_V2_V4},
    {0xF880, "set_trap_damage", {{REG_SET_FIXED, 3}}, {}, F_V2_V4}, // Normally trap damage is (700.0 * area_factor[area] * 2.0 * (0.01 * level + 0.1)); this overrides that computation. The value is specified with integer and fractional parts split up: the actual value is regsA[0] + (regsA[1] / regsA[2]).
    {0xF881, "get_pl_name", {REG}, {}, F_V2_V4},
    {0xF882, "get_pl_job", {REG}, {}, F_V2_V4},
    {0xF883, "get_player_proximity", {{REG_SET_FIXED, 2}, REG}, {}, F_V2_V4},
    {0xF884, "set_eventflag16", {INT32, REG}, {}, F_V2},
    {0xF884, "set_eventflag16", {}, {INT32, INT32}, F_V3_V4},
    {0xF885, "set_eventflag32", {INT32, REG}, {}, F_V2},
    {0xF885, "set_eventflag32", {}, {INT32, INT32}, F_V3_V4},
    {0xF886, "ba_get_place", {REG, REG}, {}, F_V2_V4},
    {0xF887, "ba_get_score", {REG, REG}, {}, F_V2_V4},
    {0xF888, "enable_win_pfx", {}, {}, F_V2_V4},
    {0xF889, "disable_win_pfx", {}, {}, F_V2_V4},
    {0xF88A, "get_player_status", {REG, REG}, {}, F_V2_V4},
    {0xF88B, "send_mail", {REG, CSTRING}, {}, F_V2},
    {0xF88B, "send_mail", {}, {REG, CSTRING}, F_V3_V4},
    {0xF88C, "get_game_version", {REG}, {}, F_V2_V4}, // Returns 2 on DCv2/PC, 3 on GC, 4 on BB
    {0xF88D, "chl_set_timerecord", {REG}, {}, F_V2 | F_V3},
    {0xF88D, "chl_set_timerecord", {REG, REG}, {}, F_V4},
    {0xF88E, "chl_get_timerecord", {REG}, {}, F_V2_V4},
    {0xF88F, "set_cmode_grave_rates", {{REG_SET_FIXED, 20}}, {}, F_V2_V4},
    {0xF890, "clear_mainwarp_all", {}, {}, F_V2_V4},
    {0xF891, "load_enemy_data", {INT32}, {}, F_V2},
    {0xF891, "load_enemy_data", {}, {INT32}, F_V3_V4},
    {0xF892, "get_physical_data", {{LABEL16, Arg::DataType::PLAYER_STATS, "stats"}}, {}, F_V2_V4},
    {0xF893, "get_attack_data", {{LABEL16, Arg::DataType::ATTACK_DATA, "attack_data"}}, {}, F_V2_V4},
    {0xF894, "get_resist_data", {{LABEL16, Arg::DataType::RESIST_DATA, "resist_data"}}, {}, F_V2_V4},
    {0xF895, "get_movement_data", {{LABEL16, Arg::DataType::MOVEMENT_DATA, "movement_data"}}, {}, F_V2_V4},
    {0xF896, "get_eventflag16", {REG, REG}, {}, F_V2_V4},
    {0xF897, "get_eventflag32", {REG, REG}, {}, F_V2_V4},
    {0xF898, "shift_left", {REG, REG}, {}, F_V2_V4},
    {0xF899, "shift_right", {REG, REG}, {}, F_V2_V4},
    {0xF89A, "get_random", {{REG_SET_FIXED, 2}, REG}, {}, F_V2_V4},
    {0xF89B, "reset_map", {}, {}, F_V2_V4},
    {0xF89C, "disp_chl_retry_menu", {REG}, {}, F_V2_V4},
    {0xF89D, "chl_reverser", {}, {}, F_V2_V4},
    {0xF89E, "ba_forbid_scape_dolls", {INT32}, {}, F_V2},
    {0xF89E, "ba_forbid_scape_dolls", {}, {INT32}, F_V3_V4},
    {0xF89F, "player_recovery", {REG}, {}, F_V2_V4}, // regA = client ID
    {0xF8A0, "disable_bosswarp_option", {}, {}, F_V2_V4},
    {0xF8A1, "enable_bosswarp_option", {}, {}, F_V2_V4},
    {0xF8A2, "is_bosswarp_opt_disabled", {REG}, {}, F_V2_V4},
    {0xF8A3, "load_serial_number_to_flag_buf", {}, {}, F_V2_V4}, // Loads 0 on BB
    {0xF8A4, "write_flag_buf_to_event_flags", {REG}, {}, F_V2_V4},
    {0xF8A5, "set_chat_callback_no_filter", {{REG_SET_FIXED, 5}}, {}, F_V2_V4},
    {0xF8A6, "set_symbol_chat_collision", {{REG_SET_FIXED, 10}}, {}, F_V2_V4},
    {0xF8A7, "set_shrink_size", {REG, {REG_SET_FIXED, 3}}, {}, F_V2_V4},
    {0xF8A8, "death_tech_lvl_up2", {INT32}, {}, F_V2},
    {0xF8A8, "death_tech_lvl_up2", {}, {INT32}, F_V3_V4},
    {0xF8A9, "vol_opt_is_dead", {REG}, {}, F_V2_V4},
    {0xF8AA, "is_there_grave_message", {REG}, {}, F_V2_V4},
    {0xF8AB, "get_ba_record", {{REG_SET_FIXED, 7}}, {}, F_V2_V4},
    {0xF8AC, "get_cmode_prize_rank", {REG}, {}, F_V2_V4},
    {0xF8AD, "get_number_of_players2", {REG}, {}, F_V2_V4},
    {0xF8AE, "party_has_name", {REG}, {}, F_V2_V4},
    {0xF8AF, "someone_has_spoken", {REG}, {}, F_V2_V4},
    {0xF8B0, "read1", {REG, REG}, {}, F_V2},
    {0xF8B0, "read1", {}, {REG, INT32}, F_V3_V4},
    {0xF8B1, "read2", {REG, REG}, {}, F_V2},
    {0xF8B1, "read2", {}, {REG, INT32}, F_V3_V4},
    {0xF8B2, "read4", {REG, REG}, {}, F_V2},
    {0xF8B2, "read4", {}, {REG, INT32}, F_V3_V4},
    {0xF8B3, "write1", {REG, REG}, {}, F_V2},
    {0xF8B3, "write1", {}, {INT32, REG}, F_V3_V4},
    {0xF8B4, "write2", {REG, REG}, {}, F_V2},
    {0xF8B4, "write2", {}, {INT32, REG}, F_V3_V4},
    {0xF8B5, "write4", {REG, REG}, {}, F_V2},
    {0xF8B5, "write4", {}, {INT32, REG}, F_V3_V4},
    {0xF8B6, "check_for_hacking", {REG}, {}, F_V2}, // Returns a bitmask of 5 different types of detectable hacking. But it only works on DCv2 - it crashes on all other versions.
    {0xF8B7, nullptr, {REG}, {}, F_V2_V4}, // TODO (DX) - Challenge mode. Appears to be timing-related; regA is expected to be in [60, 3600]. Encodes the value with encrypt_challenge_time even though it's never sent over the network and is only decrypted locally.
    {0xF8B8, "disable_retry_menu", {}, {}, F_V2_V4},
    {0xF8B9, "chl_recovery", {}, {}, F_V2_V4},
    {0xF8BA, "load_guild_card_file_creation_time_to_flag_buf", {}, {}, F_V2_V4},
    {0xF8BB, "write_flag_buf_to_event_flags2", {REG}, {}, F_V2_V4},
    {0xF8BC, "set_episode", {INT32}, {}, F_V3_V4},
    {0xF8C0, "file_dl_req", {}, {INT32, CSTRING}, F_V3}, // Sends D7
    {0xF8C0, "nop_F8C0", {}, {INT32, CSTRING}, F_V4},
    {0xF8C1, "get_dl_status", {REG}, {}, F_V3},
    {0xF8C1, "nop_F8C1", {REG}, {}, F_V4},
    {0xF8C2, "prepare_gba_rom_from_download", {}, {}, F_V3}, // Prepares to load a GBA ROM from a previous file_dl_req opcode
    {0xF8C2, "nop_F8C2", {}, {}, F_V4},
    {0xF8C3, "start_or_update_gba_joyboot", {REG}, {}, F_V3}, // One of F8C2 or F929 must be called before calling this, then this should be called repeatedly until it succeeds or fails. Return values are: 0 = not started, 1 = failed, 2 = timed out, 3 = in progress, 4 = complete
    {0xF8C3, "nop_F8C3", {REG}, {}, F_V4},
    {0xF8C4, "congrats_msg_multi_cm", {REG}, {}, F_V3},
    {0xF8C4, "nop_F8C4", {REG}, {}, F_V4},
    {0xF8C5, "stage_end_multi_cm", {REG}, {}, F_V3},
    {0xF8C5, "nop_F8C5", {REG}, {}, F_V4},
    {0xF8C6, "qexit", {}, {}, F_V3_V4},
    {0xF8C7, "use_animation", {REG, REG}, {}, F_V3_V4},
    {0xF8C8, "stop_animation", {REG}, {}, F_V3_V4},
    {0xF8C9, "run_to_coord", {{REG_SET_FIXED, 4}, REG}, {}, F_V3_V4},
    {0xF8CA, "set_slot_invincible", {REG, REG}, {}, F_V3_V4},
    {0xF8CB, "clear_slot_invincible", {REG}, {}, F_V3_V4},
    {0xF8CC, "set_slot_poison", {REG}, {}, F_V3_V4},
    {0xF8CD, "set_slot_paralyze", {REG}, {}, F_V3_V4},
    {0xF8CE, "set_slot_shock", {REG}, {}, F_V3_V4},
    {0xF8CF, "set_slot_freeze", {REG}, {}, F_V3_V4},
    {0xF8D0, "set_slot_slow", {REG}, {}, F_V3_V4},
    {0xF8D1, "set_slot_confuse", {REG}, {}, F_V3_V4},
    {0xF8D2, "set_slot_shifta", {REG}, {}, F_V3_V4},
    {0xF8D3, "set_slot_deband", {REG}, {}, F_V3_V4},
    {0xF8D4, "set_slot_jellen", {REG}, {}, F_V3_V4},
    {0xF8D5, "set_slot_zalure", {REG}, {}, F_V3_V4},
    {0xF8D6, "fleti_fixed_camera", {}, {{REG_SET_FIXED, 6}}, F_V3_V4},
    {0xF8D7, "fleti_locked_camera", {}, {INT32, {REG_SET_FIXED, 3}}, F_V3_V4},
    {0xF8D8, "default_camera_pos2", {}, {}, F_V3_V4},
    {0xF8D9, "set_motion_blur", {}, {}, F_V3_V4},
    {0xF8DA, "set_screen_bw", {}, {}, F_V3_V4},
    {0xF8DB, "get_vector_from_path", {}, {INT32, FLOAT32, FLOAT32, INT32, {REG_SET_FIXED, 4}, SCRIPT16}, F_V3_V4},
    {0xF8DC, "npc_action_string", {REG, REG, CSTRING_LABEL16}, {}, F_V3_V4},
    {0xF8DD, "get_pad_cond", {REG, REG}, {}, F_V3_V4},
    {0xF8DE, "get_button_cond", {REG, REG}, {}, F_V3_V4},
    {0xF8DF, "freeze_enemies", {}, {}, F_V3_V4},
    {0xF8E0, "unfreeze_enemies", {}, {}, F_V3_V4},
    {0xF8E1, "freeze_everything", {}, {}, F_V3_V4},
    {0xF8E2, "unfreeze_everything", {}, {}, F_V3_V4},
    {0xF8E3, "restore_hp", {REG}, {}, F_V3_V4},
    {0xF8E4, "restore_tp", {REG}, {}, F_V3_V4},
    {0xF8E5, "close_chat_bubble", {REG}, {}, F_V3_V4},
    {0xF8E6, "move_coords_object", {REG, {REG_SET_FIXED, 3}}, {}, F_V3_V4},
    {0xF8E7, "at_coords_call_ex", {{REG_SET_FIXED, 5}, REG}, {}, F_V3_V4},
    {0xF8E8, "at_coords_talk_ex", {{REG_SET_FIXED, 5}, REG}, {}, F_V3_V4},
    {0xF8E9, "walk_to_coord_call_ex", {{REG_SET_FIXED, 5}, REG}, {}, F_V3_V4},
    {0xF8EA, "col_npcinr_ex", {{REG_SET_FIXED, 6}, REG}, {}, F_V3_V4},
    {0xF8EB, "set_obj_param_ex", {{REG_SET_FIXED, 6}, REG}, {}, F_V3_V4},
    {0xF8EC, "col_plinaw_ex", {{REG_SET_FIXED, 9}, REG}, {}, F_V3_V4},
    {0xF8ED, "animation_check", {REG, REG}, {}, F_V3_V4},
    {0xF8EE, "call_image_data", {}, {INT32, {LABEL16, Arg::DataType::IMAGE_DATA}}, F_V3_V4},
    {0xF8EF, "nop_F8EF", {}, {}, F_V3_V4},
    {0xF8F0, "turn_off_bgm_p2", {}, {}, F_V3_V4},
    {0xF8F1, "turn_on_bgm_p2", {}, {}, F_V3_V4},
    {0xF8F2, nullptr, {}, {INT32, FLOAT32, FLOAT32, INT32, {REG_SET_FIXED, 4}, {LABEL16, Arg::DataType::UNKNOWN_F8F2_DATA}}, F_V3_V4}, // TODO (DX)
    {0xF8F3, "particle2", {}, {{REG_SET_FIXED, 3}, INT32, FLOAT32}, F_V3_V4},
    {0xF901, "dec2float", {REG, REG}, {}, F_V3_V4},
    {0xF902, "float2dec", {REG, REG}, {}, F_V3_V4},
    {0xF903, "flet", {REG, REG}, {}, F_V3_V4},
    {0xF904, "fleti", {REG, FLOAT32}, {}, F_V3_V4},
    {0xF908, "fadd", {REG, REG}, {}, F_V3_V4},
    {0xF909, "faddi", {REG, FLOAT32}, {}, F_V3_V4},
    {0xF90A, "fsub", {REG, REG}, {}, F_V3_V4},
    {0xF90B, "fsubi", {REG, FLOAT32}, {}, F_V3_V4},
    {0xF90C, "fmul", {REG, REG}, {}, F_V3_V4},
    {0xF90D, "fmuli", {REG, FLOAT32}, {}, F_V3_V4},
    {0xF90E, "fdiv", {REG, REG}, {}, F_V3_V4},
    {0xF90F, "fdivi", {REG, FLOAT32}, {}, F_V3_V4},
    {0xF910, "get_total_deaths", {}, {CLIENT_ID, REG}, F_V3_V4},
    {0xF911, "get_stackable_item_count", {{REG_SET_FIXED, 4}, REG}, {}, F_V3_V4}, // regsA[0] is client ID
    {0xF912, "freeze_and_hide_equip", {}, {}, F_V3_V4},
    {0xF913, "thaw_and_show_equip", {}, {}, F_V3_V4},
    {0xF914, "set_palettex_callback", {}, {CLIENT_ID, SCRIPT16}, F_V3_V4},
    {0xF915, "activate_palettex", {}, {CLIENT_ID}, F_V3_V4},
    {0xF916, "enable_palettex", {}, {CLIENT_ID}, F_V3_V4},
    {0xF917, "restore_palettex", {}, {CLIENT_ID}, F_V3_V4},
    {0xF918, "disable_palettex", {}, {CLIENT_ID}, F_V3_V4},
    {0xF919, "get_palettex_activated", {}, {CLIENT_ID, REG}, F_V3_V4},
    {0xF91A, "get_unknown_palettex_status", {}, {CLIENT_ID, INT32, REG}, F_V3_V4}, // Middle arg is unused
    {0xF91B, "disable_movement2", {}, {CLIENT_ID}, F_V3_V4},
    {0xF91C, "enable_movement2", {}, {CLIENT_ID}, F_V3_V4},
    {0xF91D, "get_time_played", {REG}, {}, F_V3_V4},
    {0xF91E, "get_guildcard_total", {REG}, {}, F_V3_V4},
    {0xF91F, "get_slot_meseta", {REG}, {}, F_V3_V4},
    {0xF920, "get_player_level", {}, {CLIENT_ID, REG}, F_V3_V4},
    {0xF921, "get_section_id", {}, {CLIENT_ID, REG}, F_V3_V4},
    {0xF922, "get_player_hp", {}, {CLIENT_ID, {REG_SET_FIXED, 4}}, F_V3_V4},
    {0xF923, "get_floor_number", {}, {CLIENT_ID, {REG_SET_FIXED, 2}}, F_V3_V4},
    {0xF924, "get_coord_player_detect", {{REG_SET_FIXED, 3}, {REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0xF925, "read_global_flag", {}, {INT32, REG}, F_V3_V4},
    {0xF926, "write_global_flag", {}, {INT32, INT32}, F_V3_V4},
    {0xF927, "item_detect_bank2", {{REG_SET_FIXED, 4}, REG}, {}, F_V3_V4},
    {0xF928, "floor_player_detect", {{REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0xF929, "prepare_gba_rom_from_disk", {}, {CSTRING}, F_V3}, // Prepares to load a GBA ROM from a local GSL file
    {0xF929, "nop_F929", {}, {CSTRING}, F_V4},
    {0xF92A, "open_pack_select", {}, {}, F_V3_V4},
    {0xF92B, "item_select", {REG}, {}, F_V3_V4},
    {0xF92C, "get_item_id", {REG}, {}, F_V3_V4},
    {0xF92D, "color_change", {}, {INT32, INT32, INT32, INT32, INT32}, F_V3_V4},
    {0xF92E, "send_statistic", {}, {INT32, INT32, INT32, INT32, INT32, INT32, INT32, INT32}, F_V3_V4},
    {0xF92F, "gba_write_identifiers", {}, {INT32, INT32}, F_V3}, // argA is ignored. If argB is 1, the game writes {system_file->creation_timestamp, current_time + rand(0, 100)} (8 bytes in total) to offset 0x2C0 in the GBA ROM data before sending it. current_time is in seconds since 1 January 2000.
    {0xF92F, "nop_F92F", {}, {INT32, INT32}, F_V4},
    {0xF930, "chat_box", {}, {INT32, INT32, INT32, INT32, INT32, CSTRING}, F_V3_V4},
    {0xF931, "chat_bubble", {}, {INT32, CSTRING}, F_V3_V4},
    {0xF932, "set_episode2", {REG}, {}, F_V3_V4},
    {0xF933, "item_create_multi_cm", {{REG_SET_FIXED, 7}}, {}, F_V3}, // regsA[1-6] form an ItemData's data1[0-5]
    {0xF933, "nop_F933", {{REG_SET_FIXED, 7}}, {}, F_V4},
    {0xF934, "scroll_text", {}, {INT32, INT32, INT32, INT32, INT32, FLOAT32, REG, CSTRING}, F_V3_V4},
    {0xF935, "gba_create_dl_graph", {}, {}, F_V3}, // Creates the download progress bar (same as the quest download progress bar)
    {0xF935, "nop_F935", {}, {}, F_V4},
    {0xF936, "gba_destroy_dl_graph", {}, {}, F_V3}, // Destroys the download progress bar
    {0xF936, "nop_F936", {}, {}, F_V4},
    {0xF937, "gba_update_dl_graph", {}, {}, F_V3}, // Updates the download progress bar
    {0xF937, "nop_F937", {}, {}, F_V4},
    {0xF938, "add_damage_to", {}, {INT32, INT32}, F_V3_V4},
    {0xF939, "item_delete3", {}, {INT32}, F_V3_V4},
    {0xF93A, "get_item_info", {}, {ITEM_ID, {REG_SET_FIXED, 12}}, F_V3_V4}, // regsB are item.data1
    {0xF93B, "item_packing1", {}, {ITEM_ID}, F_V3_V4},
    {0xF93C, "item_packing2", {}, {ITEM_ID, INT32}, F_V3_V4}, // Sends 6xD6 on BB
    {0xF93D, "get_lang_setting", {}, {REG}, F_V3_V4},
    {0xF93E, "prepare_statistic", {}, {INT32, INT32, INT32}, F_V3_V4},
    {0xF93F, "keyword_detect", {}, {}, F_V3_V4},
    {0xF940, "keyword", {}, {REG, INT32, CSTRING}, F_V3_V4},
    {0xF941, "get_guildcard_num", {}, {CLIENT_ID, REG}, F_V3_V4},
    {0xF942, "get_recent_symbol_chat", {}, {INT32, {REG_SET_FIXED, 15}}, F_V3_V4}, // argA = client ID, regsB = symbol chat data (out)
    {0xF943, "create_symbol_chat_capture_buffer", {}, {}, F_V3_V4},
    {0xF944, "get_item_stackability", {}, {ITEM_ID, REG}, F_V3_V4},
    {0xF945, "initial_floor", {}, {INT32}, F_V3_V4},
    {0xF946, "sin", {}, {REG, INT32}, F_V3_V4},
    {0xF947, "cos", {}, {REG, INT32}, F_V3_V4},
    {0xF948, "tan", {}, {REG, INT32}, F_V3_V4},
    {0xF949, "atan2_int", {}, {REG, FLOAT32, FLOAT32}, F_V3_V4},
    {0xF94A, "olga_flow_is_dead", {REG}, {}, F_V3_V4},
    {0xF94B, "particle_effect_nc", {{REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0xF94C, "player_effect_nc", {{REG_SET_FIXED, 4}}, {}, F_V3_V4},
    {0xF94D, "give_or_take_card", {{REG_SET_FIXED, 2}}, {}, F_GC_EP3}, // regsA[0] is card_id; card is given if regsA[1] >= 0, otherwise it's taken
    {0xF94D, nullptr, {}, {INT32, REG}, F_XB_V3}, // Related to voice chat. argA is a client ID; a value is read from that player's TVoiceChatClient object and (!!value) is placed in regB. This value is set by the 6xB3 command; TODO: figure out what that value represents and name this opcode appropriately
    {0xF94D, "nop_F94D", {}, {}, F_V4},
    {0xF94E, "nop_F94E", {}, {}, F_V4},
    {0xF94F, "nop_F94F", {}, {}, F_V4},
    {0xF950, "bb_p2_menu", {}, {INT32}, F_V4},
    {0xF951, "bb_map_designate", {INT8, INT8, INT8, INT8, INT8}, {}, F_V4},
    {0xF952, "bb_get_number_in_pack", {REG}, {}, F_V4},
    {0xF953, "bb_swap_item", {}, {INT32, INT32, INT32, INT32, INT32, INT32, SCRIPT16, SCRIPT16}, F_V4}, // Sends 6xD5
    {0xF954, "bb_check_wrap", {}, {INT32, REG}, F_V4},
    {0xF955, "bb_exchange_pd_item", {}, {INT32, INT32, INT32, INT32, INT32}, F_V4}, // Sends 6xD7
    {0xF956, "bb_exchange_pd_srank", {}, {INT32, INT32, INT32, INT32, INT32, INT32, INT32}, F_V4}, // Sends 6xD8
    {0xF957, "bb_exchange_pd_special", {}, {INT32, INT32, INT32, INT32, INT32, INT32, INT32, INT32}, F_V4}, // Sends 6xDA
    {0xF958, "bb_exchange_pd_percent", {}, {INT32, INT32, INT32, INT32, INT32, INT32, INT32, INT32}, F_V4}, // Sends 6xDA
    {0xF959, "bb_set_ep4_boss_can_escape", {}, {INT32}, F_V4},
    {0xF95A, "bb_is_ep4_boss_dying", {REG}, {}, F_V4},
    {0xF95B, "bb_send_6xD9", {}, {INT32, INT32, INT32, INT32, INT32, INT32}, F_V4}, // Sends 6xD9
    {0xF95C, "bb_exchange_slt", {}, {INT32, INT32, INT32, INT32}, F_V4}, // Sends 6xDE
    {0xF95D, "bb_exchange_pc", {}, {}, F_V4}, // Sends 6xDF
    {0xF95E, "bb_box_create_bp", {}, {INT32, INT32, INT32}, F_V4}, // Sends 6xE0
    {0xF95F, "bb_exchange_pt", {}, {INT32, INT32, INT32, INT32, INT32}, F_V4}, // Sends 6xE1
    {0xF960, "bb_send_6xE2", {}, {INT32}, F_V4}, // Sends 6xE2
    {0xF961, "bb_get_6xE3_status", {REG}, {}, F_V4}, // Returns 0 if 6xE3 hasn't been received, 1 if the received item is valid, 2 if the received item is invalid
};

static const unordered_map<uint16_t, const QuestScriptOpcodeDefinition*>&
opcodes_for_version(QuestScriptVersion v) {
  static array<
      unordered_map<uint16_t, const QuestScriptOpcodeDefinition*>,
      static_cast<size_t>(QuestScriptVersion::BB_V4) + 1>
      indexes;

  auto& index = indexes.at(static_cast<size_t>(v));
  if (index.empty()) {
    uint16_t vf = v_flag(v);
    for (size_t z = 0; z < sizeof(opcode_defs) / sizeof(opcode_defs[0]); z++) {
      const auto& def = opcode_defs[z];
      if (!(def.version_flags & vf)) {
        continue;
      }
      if (!index.emplace(def.opcode, &def).second) {
        throw logic_error(string_printf("duplicate definition for opcode %04hX", def.opcode));
      }
    }
  }
  return index;
}

std::string disassemble_quest_script(const void* data, size_t size, QuestScriptVersion version) {
  StringReader r(data, size);
  deque<string> lines;

  bool use_wstrs = false;
  size_t code_offset = 0;
  size_t function_table_offset = 0;
  switch (version) {
    case QuestScriptVersion::DC_NTE:
    case QuestScriptVersion::DC_V1:
    case QuestScriptVersion::DC_V2: {
      const auto& header = r.get<PSOQuestHeaderDC>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      lines.emplace_back(string_printf(".quest_num %hu", header.quest_number.load()));
      if (header.is_download) {
        lines.emplace_back(string_printf(".is_download_quest"));
      }
      lines.emplace_back(".name " + format_data_string(header.name.data(), header.name.len()));
      lines.emplace_back(".short_desc " + format_data_string(header.short_description.data(), header.short_description.len()));
      lines.emplace_back(".long_desc " + format_data_string(header.long_description.data(), header.long_description.len()));
      break;
    }
    case QuestScriptVersion::PC_V2: {
      use_wstrs = true;
      const auto& header = r.get<PSOQuestHeaderPC>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      lines.emplace_back(string_printf(".quest_num %hu", header.quest_number.load()));
      if (header.is_download) {
        lines.emplace_back(string_printf(".is_download_quest"));
      }
      lines.emplace_back(".name " + dasm_u16string(header.name));
      lines.emplace_back(".short_desc " + dasm_u16string(header.short_description));
      lines.emplace_back(".long_desc " + dasm_u16string(header.long_description));
      break;
    }
    case QuestScriptVersion::GC_NTE:
    case QuestScriptVersion::GC_V3:
    case QuestScriptVersion::GC_EP3:
    case QuestScriptVersion::XB_V3: {
      const auto& header = r.get<PSOQuestHeaderGC>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      lines.emplace_back(string_printf(".quest_num %hhu", header.quest_number));
      if (header.is_download) {
        lines.emplace_back(string_printf(".is_download_quest"));
      }
      lines.emplace_back(string_printf(".episode %hhu", header.episode));
      lines.emplace_back(".name " + format_data_string(header.name.data(), header.name.len()));
      lines.emplace_back(".short_desc " + format_data_string(header.short_description.data(), header.short_description.len()));
      lines.emplace_back(".long_desc " + format_data_string(header.long_description.data(), header.long_description.len()));
      break;
    }
    case QuestScriptVersion::BB_V4: {
      use_wstrs = true;
      const auto& header = r.get<PSOQuestHeaderBB>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      lines.emplace_back(string_printf(".quest_num %hu", header.quest_number.load()));
      lines.emplace_back(string_printf(".episode %hhu", header.episode));
      lines.emplace_back(string_printf(".max_players %hhu", header.episode));
      if (header.joinable_in_progress) {
        lines.emplace_back(".joinable_in_progress");
      }
      lines.emplace_back(".name " + dasm_u16string(header.name));
      lines.emplace_back(".short_desc " + dasm_u16string(header.short_description));
      lines.emplace_back(".long_desc " + dasm_u16string(header.long_description));
      break;
    }
    default:
      throw logic_error("invalid quest version");
  }

  const auto& opcodes = opcodes_for_version(version);
  StringReader cmd_r = r.sub(code_offset, function_table_offset - code_offset);

  struct Label {
    string name;
    uint32_t offset;
    uint32_t function_id; // 0xFFFFFFFF = no function ID
    uint64_t type_flags;
    set<size_t> references;

    Label(const string& name, uint32_t offset, int64_t function_id = -1, uint64_t type_flags = 0)
        : name(name),
          offset(offset),
          function_id(function_id),
          type_flags(type_flags) {}
    void add_data_type(Arg::DataType type) {
      this->type_flags |= (1 << static_cast<size_t>(type));
    }
    bool has_data_type(Arg::DataType type) const {
      return this->type_flags & (1 << static_cast<size_t>(type));
    }
  };

  vector<shared_ptr<Label>> function_table;
  multimap<size_t, shared_ptr<Label>> offset_to_label;
  StringReader function_table_r = r.sub(function_table_offset);
  while (!function_table_r.eof()) {
    try {
      uint32_t function_id = function_table.size();
      string name = string_printf("label%04" PRIX32, function_id);
      uint32_t offset = function_table_r.get_u32l();
      shared_ptr<Label> l(new Label(name, offset, function_id));
      if (function_id == 0) {
        l->add_data_type(Arg::DataType::SCRIPT);
      }
      function_table.emplace_back(l);
      if (l->offset < cmd_r.size()) {
        offset_to_label.emplace(l->offset, l);
      }
    } catch (const out_of_range&) {
      function_table_r.skip(function_table_r.remaining());
    }
  }

  struct DisassemblyLine {
    string line;
    size_t next_offset;

    DisassemblyLine(string&& line, size_t next_offset)
        : line(std::move(line)),
          next_offset(next_offset) {}
  };

  struct ArgStackValue {
    enum class Type {
      REG,
      REG_PTR,
      LABEL,
      INT,
      CSTRING,
    };
    Type type;
    uint32_t as_int;
    std::string as_string;

    ArgStackValue(Type type, uint32_t value) {
      this->type = type;
      this->as_int = value;
    }
    ArgStackValue(const std::string& value) {
      this->type = Type::CSTRING;
      this->as_string = value;
    }
  };

  map<size_t, DisassemblyLine> dasm_lines;
  set<size_t> pending_dasm_start_offsets;
  for (const auto& l : function_table) {
    if (l->offset < cmd_r.size()) {
      pending_dasm_start_offsets.emplace(l->offset);
    }
  }

  while (!pending_dasm_start_offsets.empty()) {
    auto dasm_start_offset_it = pending_dasm_start_offsets.begin();
    cmd_r.go(*dasm_start_offset_it);
    pending_dasm_start_offsets.erase(dasm_start_offset_it);

    vector<ArgStackValue> arg_stack_values;
    while (!cmd_r.eof() && !dasm_lines.count(cmd_r.where())) {
      size_t opcode_start_offset = cmd_r.where();
      string dasm_line;
      try {
        uint16_t opcode = cmd_r.get_u8();
        if ((opcode & 0xFE) == 0xF8) {
          opcode = (opcode << 8) | cmd_r.get_u8();
        }

        const QuestScriptOpcodeDefinition* def = nullptr;
        try {
          def = opcodes.at(opcode);
        } catch (const out_of_range&) {
        }

        if (def == nullptr) {
          dasm_line = string_printf(".unknown %04hX", opcode);
        } else {
          dasm_line = def->name ? def->name : string_printf("[%04hX]", opcode);
          if (!def->imm_args.empty()) {
            dasm_line.resize(0x20, ' ');
            bool is_first_arg = true;
            for (const auto& arg : def->imm_args) {
              using Type = QuestScriptOpcodeDefinition::Argument::Type;
              string dasm_arg;
              switch (arg.type) {
                case Type::LABEL16:
                case Type::LABEL32: {
                  uint32_t label_id = (arg.type == Type::LABEL32) ? cmd_r.get_u32l() : cmd_r.get_u16l();
                  if (def->preserve_args_list) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::LABEL, label_id);
                  }
                  if (label_id >= function_table.size()) {
                    dasm_arg = string_printf("label%04" PRIX32 " /* invalid */", label_id);
                  } else {
                    auto& l = function_table.at(label_id);
                    dasm_arg = string_printf("label%04" PRIX32 " /* %04" PRIX32 " */", label_id, l->offset);
                    l->references.emplace(opcode_start_offset);
                    l->add_data_type(arg.data_type);
                    if (arg.data_type == Arg::DataType::SCRIPT) {
                      pending_dasm_start_offsets.emplace(l->offset);
                    }
                  }
                  break;
                }
                case Type::LABEL16_SET: {
                  if (def->preserve_args_list) {
                    throw logic_error("LABEL16_SET cannot be pushed to arg stack");
                  }
                  uint8_t num_functions = cmd_r.get_u8();
                  for (size_t z = 0; z < num_functions; z++) {
                    dasm_arg += (dasm_arg.empty() ? "(" : ", ");
                    uint32_t label_id = cmd_r.get_u16l();
                    if (label_id >= function_table.size()) {
                      dasm_arg += string_printf("function%04" PRIX32 " /* invalid */", label_id);
                    } else {
                      auto& l = function_table.at(label_id);
                      dasm_arg += string_printf("label%04" PRIX32 " /* %04" PRIX32 " */", label_id, l->offset);
                      l->references.emplace(opcode_start_offset);
                      l->add_data_type(arg.data_type);
                      if (arg.data_type == Arg::DataType::SCRIPT) {
                        pending_dasm_start_offsets.emplace(l->offset);
                      }
                    }
                  }
                  if (dasm_arg.empty()) {
                    dasm_arg = "()";
                  } else {
                    dasm_arg += ")";
                  }
                  break;
                }
                case Type::REG: {
                  uint8_t reg = cmd_r.get_u8();
                  if (def->preserve_args_list) {
                    arg_stack_values.emplace_back((def->opcode == 0x004C) ? ArgStackValue::Type::REG_PTR : ArgStackValue::Type::REG, reg);
                  }
                  dasm_arg = string_printf("r%hhu", reg);
                  break;
                }
                case Type::REG_SET: {
                  if (def->preserve_args_list) {
                    throw logic_error("REG_SET cannot be pushed to arg stack");
                  }
                  uint8_t num_regs = cmd_r.get_u8();
                  for (size_t z = 0; z < num_regs; z++) {
                    dasm_arg += string_printf("%sr%hhu", (dasm_arg.empty() ? "(" : ", "), cmd_r.get_u8());
                  }
                  if (dasm_arg.empty()) {
                    dasm_arg = "()";
                  } else {
                    dasm_arg += ")";
                  }
                  break;
                }
                case Type::REG_SET_FIXED: {
                  uint8_t first_reg = cmd_r.get_u8();
                  if (def->preserve_args_list) {
                    throw logic_error("REG_SET_FIXED cannot be pushed to arg stack");
                  }
                  dasm_arg = string_printf("r%hhu-r%hhu", first_reg, static_cast<uint8_t>(first_reg + arg.count - 1));
                  break;
                }
                case Type::INT8: {
                  uint8_t v = cmd_r.get_u8();
                  if (def->preserve_args_list) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, v);
                  }
                  dasm_arg = string_printf("0x%02hhX", v);
                  break;
                }
                case Type::INT16: {
                  uint16_t v = cmd_r.get_u16l();
                  if (def->preserve_args_list) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, v);
                  }
                  dasm_arg = string_printf("0x%04hX", v);
                  break;
                }
                case Type::INT32: {
                  uint32_t v = cmd_r.get_u32l();
                  if (def->preserve_args_list) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, v);
                  }
                  dasm_arg = string_printf("0x%08" PRIX32, v);
                  break;
                }
                case Type::FLOAT32: {
                  float v = cmd_r.get_f32l();
                  if (def->preserve_args_list) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, as_type<uint32_t>(v));
                  }
                  dasm_arg = string_printf("%g", v);
                  break;
                }
                case Type::CSTRING:
                  if (use_wstrs) {
                    u16string s;
                    for (char16_t ch = cmd_r.get_u16l(); ch; ch = cmd_r.get_u16l()) {
                      s.push_back(ch);
                    }
                    if (def->preserve_args_list) {
                      arg_stack_values.emplace_back(encode_sjis(s));
                    }
                    dasm_arg = dasm_u16string(s.data(), s.size());
                  } else {
                    string s = cmd_r.get_cstr();
                    if (def->preserve_args_list) {
                      arg_stack_values.emplace_back(s);
                    }
                    dasm_arg = format_data_string(s);
                  }
                  break;
                default:
                  throw logic_error("invalid argument type");
              }
              if (!is_first_arg) {
                dasm_line += ", ";
              } else {
                is_first_arg = false;
              }
              dasm_line += dasm_arg;
            }
          }

          if (!def->stack_args.empty()) {
            if (!def->imm_args.empty()) {
              throw logic_error("opcode has both imm_args and stack_args");
            }
            dasm_line.resize(0x20, ' ');
            dasm_line += "... ";

            if (def->stack_args.size() != arg_stack_values.size()) {
              dasm_line += string_printf("/* matching error: expected %zu arguments, received %zu arguments */",
                  def->stack_args.size(), arg_stack_values.size());
            } else {
              bool is_first_arg = true;
              for (size_t z = 0; z < def->stack_args.size(); z++) {
                const auto& arg_def = def->stack_args[z];
                const auto& arg_value = arg_stack_values[z];

                string dasm_arg;
                switch (arg_def.type) {
                  case Arg::Type::LABEL16:
                  case Arg::Type::LABEL32:
                    switch (arg_value.type) {
                      case ArgStackValue::Type::REG:
                        dasm_arg = string_printf("r%" PRIu32 "/* warning: cannot determine label data type */", arg_value.as_int);
                        break;
                      case ArgStackValue::Type::LABEL:
                      case ArgStackValue::Type::INT:
                        dasm_arg = string_printf("label%04" PRIX32, arg_value.as_int);
                        try {
                          auto l = function_table.at(arg_value.as_int);
                          l->add_data_type(arg_def.data_type);
                          l->references.emplace(opcode_start_offset);
                        } catch (const out_of_range&) {
                        }
                        break;
                      default:
                        dasm_arg = "/* invalid-type */";
                    }
                    break;
                  case Arg::Type::REG:
                  case Arg::Type::REG32:
                    switch (arg_value.type) {
                      case ArgStackValue::Type::REG:
                        dasm_arg = string_printf("regs[r%" PRIu32 "]", arg_value.as_int);
                        break;
                      case ArgStackValue::Type::INT:
                        dasm_arg = string_printf("r%" PRIu32, arg_value.as_int);
                        break;
                      default:
                        dasm_arg = "/* invalid-type */";
                    }
                    break;
                  case Arg::Type::REG_SET_FIXED:
                  case Arg::Type::REG32_SET_FIXED:
                    switch (arg_value.type) {
                      case ArgStackValue::Type::REG:
                        dasm_arg = string_printf("regs[r%" PRIu32 "]-regs[r%" PRIu32 "+%hhu]", arg_value.as_int, arg_value.as_int, static_cast<uint8_t>(arg_def.count - 1));
                        break;
                      case ArgStackValue::Type::INT:
                        dasm_arg = string_printf("r%" PRIu32 "-r%hhu", arg_value.as_int, static_cast<uint8_t>(arg_value.as_int + arg_def.count - 1));
                        break;
                      default:
                        dasm_arg = "/* invalid-type */";
                    }
                    break;
                  case Arg::Type::INT8:
                  case Arg::Type::INT16:
                  case Arg::Type::INT32:
                    switch (arg_value.type) {
                      case ArgStackValue::Type::REG:
                        dasm_arg = string_printf("r%" PRIu32, arg_value.as_int);
                        break;
                      case ArgStackValue::Type::REG_PTR:
                        dasm_arg = string_printf("&r%" PRIu32, arg_value.as_int);
                        break;
                      case ArgStackValue::Type::INT:
                        dasm_arg = string_printf("0x%" PRIX32 " /* %" PRIu32 " */", arg_value.as_int, arg_value.as_int);
                        break;
                      default:
                        dasm_arg = "/* invalid-type */";
                    }
                    break;
                  case Arg::Type::FLOAT32:
                    switch (arg_value.type) {
                      case ArgStackValue::Type::REG:
                        dasm_arg = string_printf("(float)r%" PRIu32, arg_value.as_int);
                        break;
                      case ArgStackValue::Type::INT:
                        dasm_arg = string_printf("%g", as_type<float>(arg_value.as_int));
                        break;
                      default:
                        dasm_arg = "/* invalid-type */";
                    }
                    break;
                  case Arg::Type::CSTRING:
                    if (arg_value.type == ArgStackValue::Type::CSTRING) {
                      dasm_arg = format_data_string(arg_value.as_string);
                    } else {
                      dasm_arg = "/* invalid-type */";
                    }
                    break;
                  case Arg::Type::LABEL16_SET:
                  case Arg::Type::REG_SET:
                  default:
                    throw logic_error("set-type arg found on arg stack");
                }

                if (!is_first_arg) {
                  dasm_line += ", ";
                } else {
                  is_first_arg = false;
                }
                dasm_line += dasm_arg;
              }
            }
          }

          if (!def->preserve_args_list) {
            arg_stack_values.clear();
          }
        }
      } catch (const exception& e) {
        dasm_line = string_printf(".failed (%s)", e.what());
      }

      string hex_data = format_data_string(cmd_r.preadx(opcode_start_offset, cmd_r.where() - opcode_start_offset), nullptr, FormatDataFlags::HEX_ONLY);
      if (hex_data.size() > 14) {
        hex_data.resize(12);
        hex_data += "...";
      }
      hex_data.resize(16, ' ');

      dasm_lines.emplace(
          opcode_start_offset,
          DisassemblyLine(
              string_printf("  %04zX  %s  %s", opcode_start_offset, hex_data.c_str(), dasm_line.c_str()), cmd_r.where()));
    }
  }

  auto label_it = offset_to_label.begin();
  while (label_it != offset_to_label.end()) {
    auto l = label_it->second;
    label_it++;
    size_t size = ((label_it == offset_to_label.end()) ? cmd_r.size() : label_it->second->offset) - l->offset;
    if (size > 0) {
      lines.emplace_back();
    }
    if (l->function_id == 0) {
      lines.emplace_back("start:");
    }
    lines.emplace_back(string_printf("label%04" PRIX32 ":", l->function_id));
    if (l->references.size() == 1) {
      lines.emplace_back(string_printf("  // Referenced by instruction at %04zX", *l->references.begin()));
    } else if (!l->references.empty()) {
      vector<string> tokens;
      tokens.reserve(l->references.size());
      for (size_t reference_offset : l->references) {
        tokens.emplace_back(string_printf("%04zX", reference_offset));
      }
      lines.emplace_back("  // Referenced by instructions at " + join(tokens, ", "));
    }

    auto print_as_struct = [&]<Arg::DataType data_type, typename StructT>(function<void(const StructT&)> print_fn) {
      if (l->has_data_type(data_type)) {
        if (size >= sizeof(StructT)) {
          print_fn(cmd_r.pget<StructT>(l->offset));
          if (size > sizeof(StructT)) {
            size_t struct_end_offset = l->offset + sizeof(StructT);
            size_t remaining_size = size - sizeof(StructT);
            lines.emplace_back("  // Extra data after structure");
            lines.emplace_back(format_and_indent_data(cmd_r.pgetv(struct_end_offset, remaining_size), remaining_size, struct_end_offset));
          }
        } else {
          lines.emplace_back(string_printf("  // As raw data (0x%zX bytes; too small for referenced type)", size));
          lines.emplace_back(format_and_indent_data(cmd_r.pgetv(l->offset, size), size, l->offset));
        }
      }
    };

    if (l->type_flags == 0) {
      lines.emplace_back(string_printf("  // Could not determine data type; disassembling as code"));
      l->add_data_type(Arg::DataType::SCRIPT);
    }

    // Print data interpretations of the label (if any)
    if (l->has_data_type(Arg::DataType::DATA)) {
      lines.emplace_back(string_printf("  // As raw data (0x%zX bytes)", size));
      lines.emplace_back(format_and_indent_data(cmd_r.pgetv(l->offset, size), size, l->offset));
    }
    if (l->has_data_type(Arg::DataType::CSTRING)) {
      lines.emplace_back(string_printf("  // As C string (0x%zX bytes)", size));
      string data;
      if (use_wstrs) {
        u16string wdata(reinterpret_cast<const char16_t*>(cmd_r.pgetv(l->offset, size)), size >> 1);
        strip_trailing_zeroes(wdata);
        data = encode_sjis(wdata);
      } else {
        data = cmd_r.pread(l->offset, size);
        strip_trailing_zeroes(data);
      }
      string formatted = format_data_string(data);
      lines.emplace_back(string_printf("  %04" PRIX32 "  %s", l->offset, formatted.c_str()));
    }
    print_as_struct.template operator()<Arg::DataType::PLAYER_VISUAL_CONFIG, PlayerVisualConfig>([&](const PlayerVisualConfig& visual) -> void {
      lines.emplace_back("  // As PlayerVisualConfig");
      string name = format_data_string(visual.name);
      lines.emplace_back(string_printf("  %04zX  name         %s", l->offset + offsetof(PlayerVisualConfig, name), name.c_str()));
      lines.emplace_back(string_printf("  %04zX  name_color   %08" PRIX32, l->offset + offsetof(PlayerVisualConfig, name_color), visual.name_color.load()));
      lines.emplace_back(string_printf("  %04zX  a2           %016" PRIX64, l->offset + offsetof(PlayerVisualConfig, unknown_a2), visual.unknown_a2.load()));
      lines.emplace_back(string_printf("  %04zX  extra_model  %02hhX", l->offset + offsetof(PlayerVisualConfig, extra_model), visual.extra_model));
      string unused = format_data_string(visual.unused.data(), visual.unused.bytes());
      lines.emplace_back(string_printf("  %04zX  unused       %s", l->offset + offsetof(PlayerVisualConfig, unused), unused.c_str()));
      lines.emplace_back(string_printf("  %04zX  a3           %08" PRIX32, l->offset + offsetof(PlayerVisualConfig, unknown_a3), visual.unknown_a3.load()));
      string secid_name = name_for_section_id(visual.section_id);
      lines.emplace_back(string_printf("  %04zX  section_id   %02hhX (%s)", l->offset + offsetof(PlayerVisualConfig, section_id), visual.section_id, secid_name.c_str()));
      lines.emplace_back(string_printf("  %04zX  char_class   %02hhX (%s)", l->offset + offsetof(PlayerVisualConfig, char_class), visual.char_class, name_for_char_class(visual.char_class)));
      lines.emplace_back(string_printf("  %04zX  v2_flags     %02hhX", l->offset + offsetof(PlayerVisualConfig, v2_flags), visual.v2_flags));
      lines.emplace_back(string_printf("  %04zX  version      %02hhX", l->offset + offsetof(PlayerVisualConfig, version), visual.version));
      lines.emplace_back(string_printf("  %04zX  v1_flags     %08" PRIX32, l->offset + offsetof(PlayerVisualConfig, v1_flags), visual.v1_flags.load()));
      lines.emplace_back(string_printf("  %04zX  costume      %04hX", l->offset + offsetof(PlayerVisualConfig, costume), visual.costume.load()));
      lines.emplace_back(string_printf("  %04zX  skin         %04hX", l->offset + offsetof(PlayerVisualConfig, skin), visual.skin.load()));
      lines.emplace_back(string_printf("  %04zX  face         %04hX", l->offset + offsetof(PlayerVisualConfig, face), visual.face.load()));
      lines.emplace_back(string_printf("  %04zX  head         %04hX", l->offset + offsetof(PlayerVisualConfig, head), visual.head.load()));
      lines.emplace_back(string_printf("  %04zX  hair         %04hX", l->offset + offsetof(PlayerVisualConfig, hair), visual.hair.load()));
      lines.emplace_back(string_printf("  %04zX  hair_color   %04hX, %04hX, %04hX", l->offset + offsetof(PlayerVisualConfig, hair_r), visual.hair_r.load(), visual.hair_g.load(), visual.hair_b.load()));
      lines.emplace_back(string_printf("  %04zX  proportion   %g, %g", l->offset + offsetof(PlayerVisualConfig, proportion_x), visual.proportion_x.load(), visual.proportion_y.load()));
    });
    print_as_struct.template operator()<Arg::DataType::PLAYER_STATS, PlayerStats>([&](const PlayerStats& stats) -> void {
      lines.emplace_back("  // As PlayerStats");
      lines.emplace_back(string_printf("  %04zX  atp          %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.atp), stats.char_stats.atp.load(), stats.char_stats.atp.load()));
      lines.emplace_back(string_printf("  %04zX  mst          %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.mst), stats.char_stats.mst.load(), stats.char_stats.mst.load()));
      lines.emplace_back(string_printf("  %04zX  evp          %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.evp), stats.char_stats.evp.load(), stats.char_stats.evp.load()));
      lines.emplace_back(string_printf("  %04zX  hp           %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.hp), stats.char_stats.hp.load(), stats.char_stats.hp.load()));
      lines.emplace_back(string_printf("  %04zX  dfp          %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.dfp), stats.char_stats.dfp.load(), stats.char_stats.dfp.load()));
      lines.emplace_back(string_printf("  %04zX  ata          %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.ata), stats.char_stats.ata.load(), stats.char_stats.ata.load()));
      lines.emplace_back(string_printf("  %04zX  lck          %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.lck), stats.char_stats.lck.load(), stats.char_stats.lck.load()));
      lines.emplace_back(string_printf("  %04zX  a1           %04hX /* %hu */", l->offset + offsetof(PlayerStats, unknown_a1), stats.unknown_a1.load(), stats.unknown_a1.load()));
      lines.emplace_back(string_printf("  %04zX  a2           %08" PRIX32 " /* %g */", l->offset + offsetof(PlayerStats, unknown_a2), stats.unknown_a2.load_raw(), stats.unknown_a2.load()));
      lines.emplace_back(string_printf("  %04zX  a3           %08" PRIX32 " /* %g */", l->offset + offsetof(PlayerStats, unknown_a3), stats.unknown_a3.load_raw(), stats.unknown_a3.load()));
      lines.emplace_back(string_printf("  %04zX  level        %08" PRIX32 " /* level %" PRIu32 " */", l->offset + offsetof(PlayerStats, level), stats.level.load(), stats.level.load() + 1));
      lines.emplace_back(string_printf("  %04zX  experience   %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(PlayerStats, experience), stats.experience.load(), stats.experience.load()));
      lines.emplace_back(string_printf("  %04zX  meseta       %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(PlayerStats, meseta), stats.meseta.load(), stats.meseta.load()));
    });
    print_as_struct.template operator()<Arg::DataType::RESIST_DATA, ResistData>([&](const ResistData& resist) -> void {
      lines.emplace_back("  // As ResistData");
      lines.emplace_back(string_printf("  %04zX  evp_bonus    %04hX /* %hu */", l->offset + offsetof(ResistData, evp_bonus), resist.evp_bonus.load(), resist.evp_bonus.load()));
      lines.emplace_back(string_printf("  %04zX  a1           %04hX /* %hu */", l->offset + offsetof(ResistData, unknown_a1), resist.unknown_a1.load(), resist.unknown_a1.load()));
      lines.emplace_back(string_printf("  %04zX  a2           %04hX /* %hu */", l->offset + offsetof(ResistData, unknown_a2), resist.unknown_a2.load(), resist.unknown_a2.load()));
      lines.emplace_back(string_printf("  %04zX  a3           %04hX /* %hu */", l->offset + offsetof(ResistData, unknown_a3), resist.unknown_a3.load(), resist.unknown_a3.load()));
      lines.emplace_back(string_printf("  %04zX  a4           %04hX /* %hu */", l->offset + offsetof(ResistData, unknown_a4), resist.unknown_a4.load(), resist.unknown_a4.load()));
      lines.emplace_back(string_printf("  %04zX  a5           %04hX /* %hu */", l->offset + offsetof(ResistData, unknown_a5), resist.unknown_a5.load(), resist.unknown_a5.load()));
      lines.emplace_back(string_printf("  %04zX  a6           %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a6), resist.unknown_a6.load(), resist.unknown_a6.load()));
      lines.emplace_back(string_printf("  %04zX  a7           %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a7), resist.unknown_a7.load(), resist.unknown_a7.load()));
      lines.emplace_back(string_printf("  %04zX  a8           %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a8), resist.unknown_a8.load(), resist.unknown_a8.load()));
      lines.emplace_back(string_printf("  %04zX  a9           %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a9), resist.unknown_a9.load(), resist.unknown_a9.load()));
      lines.emplace_back(string_printf("  %04zX  dfp_bonus    %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, dfp_bonus), resist.dfp_bonus.load(), resist.dfp_bonus.load()));
    });
    print_as_struct.template operator()<Arg::DataType::ATTACK_DATA, AttackData>([&](const AttackData& attack) -> void {
      lines.emplace_back("  // As AttackData");
      lines.emplace_back(string_printf("  %04zX  a1           %04hX /* %hd */", l->offset + offsetof(AttackData, unknown_a1), attack.unknown_a1.load(), attack.unknown_a1.load()));
      lines.emplace_back(string_printf("  %04zX  a2           %04hX /* %hd */", l->offset + offsetof(AttackData, unknown_a2), attack.unknown_a2.load(), attack.unknown_a2.load()));
      lines.emplace_back(string_printf("  %04zX  a3           %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a3), attack.unknown_a3.load(), attack.unknown_a3.load()));
      lines.emplace_back(string_printf("  %04zX  a4           %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a4), attack.unknown_a4.load(), attack.unknown_a4.load()));
      lines.emplace_back(string_printf("  %04zX  a5           %08" PRIX32 " /* %g */", l->offset + offsetof(AttackData, unknown_a5), attack.unknown_a5.load_raw(), attack.unknown_a5.load()));
      lines.emplace_back(string_printf("  %04zX  a6           %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a6), attack.unknown_a6.load(), attack.unknown_a6.load()));
      lines.emplace_back(string_printf("  %04zX  a7           %08" PRIX32 " /* %g */", l->offset + offsetof(AttackData, unknown_a7), attack.unknown_a7.load_raw(), attack.unknown_a7.load()));
      lines.emplace_back(string_printf("  %04zX  a8           %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a8), attack.unknown_a8.load(), attack.unknown_a8.load()));
      lines.emplace_back(string_printf("  %04zX  a9           %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a9), attack.unknown_a9.load(), attack.unknown_a9.load()));
      lines.emplace_back(string_printf("  %04zX  a10          %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a10), attack.unknown_a10.load(), attack.unknown_a10.load()));
      lines.emplace_back(string_printf("  %04zX  a11          %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a11), attack.unknown_a11.load(), attack.unknown_a11.load()));
      lines.emplace_back(string_printf("  %04zX  a12          %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a12), attack.unknown_a12.load(), attack.unknown_a12.load()));
      lines.emplace_back(string_printf("  %04zX  a13          %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a13), attack.unknown_a13.load(), attack.unknown_a13.load()));
      lines.emplace_back(string_printf("  %04zX  a14          %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a14), attack.unknown_a14.load(), attack.unknown_a14.load()));
      lines.emplace_back(string_printf("  %04zX  a15          %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a15), attack.unknown_a15.load(), attack.unknown_a15.load()));
      lines.emplace_back(string_printf("  %04zX  a16          %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a16), attack.unknown_a16.load(), attack.unknown_a16.load()));
    });
    print_as_struct.template operator()<Arg::DataType::MOVEMENT_DATA, MovementData>([&](const MovementData& movement) -> void {
      lines.emplace_back("  // As MovementData");
      for (size_t z = 0; z < 6; z++) {
        size_t offset = l->offset + z * sizeof(movement.unknown_a1[0]);
        lines.emplace_back(string_printf("  %04zX  a1[%zu]        %08" PRIX32 " /* %g */",
            offset, z, movement.unknown_a1[z].load_raw(), movement.unknown_a1[z].load()));
      }
      for (size_t z = 0; z < 6; z++) {
        size_t offset = l->offset + sizeof(movement.unknown_a1) + z * sizeof(movement.unknown_a2[0]);
        lines.emplace_back(string_printf("  %04zX  a2[%zu]        %08" PRIX32 " /* %g */",
            offset, z, movement.unknown_a2[z].load_raw(), movement.unknown_a2[z].load()));
      }
    });
    if (l->has_data_type(Arg::DataType::IMAGE_DATA)) {
      const void* data = cmd_r.pgetv(l->offset, size);
      auto decompressed = prs_decompress_with_meta(data, size);
      lines.emplace_back(string_printf("  // As decompressed image data (0x%zX bytes)", decompressed.data.size()));
      lines.emplace_back(format_and_indent_data(decompressed.data.data(), decompressed.data.size(), 0));
      if (decompressed.input_bytes_used < size) {
        size_t compressed_end_offset = l->offset + decompressed.input_bytes_used;
        size_t remaining_size = size - decompressed.input_bytes_used;
        lines.emplace_back("  // Extra data after compressed data");
        lines.emplace_back(format_and_indent_data(cmd_r.pgetv(compressed_end_offset, remaining_size), remaining_size, compressed_end_offset));
      }
    }
    if (l->has_data_type(Arg::DataType::UNKNOWN_F8F2_DATA)) {
      StringReader r = cmd_r.sub(l->offset, size);
      lines.emplace_back("  // As F8F2 entries");
      while (r.remaining() >= sizeof(UnknownF8F2Entry)) {
        size_t offset = r.where() + cmd_r.where();
        const auto& e = r.get<UnknownF8F2Entry>();
        lines.emplace_back(string_printf("  %04zX  entry        %g, %g, %g, %g", offset, e.unknown_a1[0].load(), e.unknown_a1[1].load(), e.unknown_a1[2].load(), e.unknown_a1[3].load()));
      }
      if (r.remaining() > 0) {
        size_t struct_end_offset = l->offset + r.where();
        size_t remaining_size = r.remaining();
        lines.emplace_back("  // Extra data after structures");
        lines.emplace_back(format_and_indent_data(r.getv(remaining_size), remaining_size, struct_end_offset));
      }
    }
    if (l->has_data_type(Arg::DataType::SCRIPT)) {
      for (size_t z = l->offset; z < l->offset + size;) {
        const auto& l = dasm_lines.at(z);
        lines.emplace_back(l.line);
        if (l.next_offset <= z) {
          throw logic_error("line points backward or to itself");
        }
        z = l.next_offset;
      }
    }
  }

  lines.emplace_back(); // Add a \n on the end
  return join(lines, "\n");
}