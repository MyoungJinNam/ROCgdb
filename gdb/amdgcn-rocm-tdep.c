/* Target-dependent code for the ROCm amdgcn architecture.

   Copyright (C) 2019-2021 Free Software Foundation, Inc.
   Copyright (C) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"

#include "amdgcn-rocm-tdep.h"
#include "arch-utils.h"
#include "dwarf2/frame.h"
#include "frame-base.h"
#include "frame-unwind.h"
#include "gdbarch.h"
#include "gdbsupport/gdb_unique_ptr.h"
#include "inferior.h"
#include "objfiles.h"
#include "osabi.h"
#include "producer.h"
#include "reggroups.h"
#include "rocm-tdep.h"

#include <iterator>
#include <regex>
#include <string>

/* Bit mask of the address space information in the core address.  */
constexpr CORE_ADDR AMDGCN_ADDRESS_SPACE_MASK = 0xff00000000000000;

/* Bit offset from the start of the core address
   that represent the address space information.  */
constexpr unsigned int AMDGCN_ADDRESS_SPACE_BIT_OFFSET = 56;

bool
rocm_is_amdgcn_gdbarch (struct gdbarch *arch)
{
  gdb_assert (arch != nullptr);
  return gdbarch_bfd_arch_info (arch)->arch == bfd_arch_amdgcn;
}

amdgcn_gdbarch_tdep *
get_amdgcn_gdbarch_tdep (gdbarch *arch)
{
  return static_cast<amdgcn_gdbarch_tdep *> (gdbarch_tdep (arch));
}

/* Return the name of register REGNUM.  */
static const char *
amdgcn_register_name (struct gdbarch *gdbarch, int regnum)
{
  amd_dbgapi_wave_id_t wave_id = get_amd_dbgapi_wave_id (inferior_ptid);
  amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);

  amd_dbgapi_register_exists_t register_exists;
  if (amd_dbgapi_wave_register_exists (wave_id, tdep->register_ids[regnum],
				       &register_exists)
	!= AMD_DBGAPI_STATUS_SUCCESS
      || register_exists != AMD_DBGAPI_REGISTER_PRESENT)
    return "";

  return tdep->register_names[regnum].c_str ();
}

static int
amdgcn_dwarf_reg_to_regnum (struct gdbarch *gdbarch, int dwarf_reg)
{
  amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);

  if (dwarf_reg < tdep->dwarf_regnum_to_gdb_regnum.size ())
    return tdep->dwarf_regnum_to_gdb_regnum[dwarf_reg];

  return -1;
}

static enum return_value_convention
amdgcn_return_value (struct gdbarch *gdbarch, struct value *function,
		     struct type *type, struct regcache *regcache,
		     gdb_byte *readbuf, const gdb_byte *writebuf)
{
  return RETURN_VALUE_STRUCT_CONVENTION;
}

/* Return the gdb register type from a C style type string.  The type string
   syntax is defined by the following BNF syntax:

       type          ::= integer_type
		       | float_type
		       | function_type
		       | flags_type
		       | array_type
       array_type    ::= ( integer_type
			 | float_type
			 | function_type
			 | flags_type
			 ) "[" element_count "]"
       element_count ::= DECIMAL_NUMBER
       integer_type  ::= "uint32_t"
		       | "uint64_t"
       float_type    ::= "float"
		       | "double"
       function_type ::= "void(void)"
       flags_type    ::= ( "flags32_t"
			 | "flags64_t"
			 )
			 type_name
			 [ "{" [ fields ] "}" ]
       fields        ::= field ";" [ fields ]
       field         ::= "bool" field_name
			 "@" bit_position
		       | ( integer_type
			 | enum_type
			 )
			 field_name
			 "@" bit_position
			 "-" bit_position
       field_name    ::= IDENTIFIER
       enum_type     ::= "enum" type_name
			 [ "{" [ enum_values ] "}" ]
       enum_values   ::= enum_value [ "," enum_values ]
       enum_value    ::= enum_name "=" enum_ordinal
       enum_name     ::= IDENTIFIER
       enum_ordinal  ::= DECIMAL_NUMBER
       type_name     ::= IDENTIFIER
       bit_position  ::= DECIMAL_NUMBER

   ``IDENTIFIER`` is a string starting with an alphabetic character followed by
   zero or more alphebetic, numeric, "_", or "." characters.

   ``DECIMAL_NUMBER`` is a decimal C integral literal.

   Whitespace is allowed between lexical tokens.

   The type size matches the size of the register.  uint32_t, float, and
   flags32_t types are 4 bytes.  uint64_t, double, and flags64_t types
   are 8 bytes.  void(void) is the size of a global address.
*/
static struct type *gdb_type_from_type_name (struct gdbarch *gdbarch,
					     const std::string &type_name);

static struct type *
amdgcn_enum_type (struct gdbarch *gdbarch, int bits,
		  const std::string &type_name, const std::string &fields)
{
  gdb_assert (bits == 32 || bits == 64);
  amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);

  auto it = tdep->type_map.find (type_name);
  if (it != tdep->type_map.end ())
    return it->second;

  /* If the enum type is not defined, return a default unsigned type.  */
  if (fields.empty ())
    return bits == 32 ? builtin_type (gdbarch)->builtin_uint32
		      : builtin_type (gdbarch)->builtin_uint64;

  struct type *enum_type
    = arch_type (gdbarch, TYPE_CODE_ENUM, bits, type_name.c_str ());

  std::regex regex ("(\\w+)\\s*=\\s*(\\d+)\\s*(,|$)");
  auto begin = std::sregex_iterator (fields.begin (), fields.end (), regex);
  auto end = std::sregex_iterator ();
  size_t count = std::distance (begin, end);

  enum_type->set_num_fields (count);
  enum_type->set_fields (
    (struct field *) TYPE_ZALLOC (enum_type, sizeof (struct field) * count));
  enum_type->set_is_unsigned (true);

  size_t i = 0;
  for (auto item = begin; item != end; ++item, ++i)
    {
      std::smatch match = *item;

      auto enumval = std::stoul (match[2].str ());
      if (bits == 32 && enumval > std::numeric_limits<uint32_t>::max ())
	TYPE_LENGTH (enum_type) = (bits = 64) / TARGET_CHAR_BIT;

      enum_type->field (i).set_name (xstrdup (match[1].str ().c_str ()));
      enum_type->field (i).set_loc_enumval (enumval);
    }

  tdep->type_map.emplace (type_name, enum_type);
  return enum_type;
}

static struct type *
amdgcn_flags_type (struct gdbarch *gdbarch, int bits,
		   const std::string &type_name, const std::string &fields)
{
  gdb_assert (bits == 32 || bits == 64);
  amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);

  auto it = tdep->type_map.find (type_name);
  if (it != tdep->type_map.end ())
    return it->second;

  /* If the flags type is not defined, return a default unsigned type.  */
  if (fields.empty ())
    return bits == 32 ? builtin_type (gdbarch)->builtin_uint32
		      : builtin_type (gdbarch)->builtin_uint64;

  struct type *flags_type
    = arch_flags_type (gdbarch, type_name.c_str (), bits);

  std::regex regex (
    string_printf (/* 1: field_type  */
		   "(bool|uint%d_t|enum\\s+\\w+\\s*(\\{[^\\}]*\\})?)\\s+"
		   /* 3: field_name */ "([[:alnum:]_\\.]+)\\s+"
		   /* 4,5: bit_position(s) */ "@(\\d+)(-\\d+)?\\s*;",
		   bits));

  for (std::sregex_iterator item
       = std::sregex_iterator (fields.begin (), fields.end (), regex);
       item != std::sregex_iterator (); ++item)
    {
      std::smatch match = *item;
      std::string field_type = match[1].str ();
      std::string field_name = match[3].str ();

      if (field_type == "bool")
	{
	  int pos = std::stoi (match[4].str ());
	  append_flags_type_flag (flags_type, pos, field_name.c_str ());
	}
      else if (match[5].length ()) /* non-boolean fields require a start bit
				      position and an end bit position.  */
	{
	  int start = std::stoi (match[4].str ());
	  int end = std::stoi (match[5].str ().substr (1));
	  append_flags_type_field (flags_type, start, end - start + 1,
				   gdb_type_from_type_name (gdbarch,
							    field_type),
				   field_name.c_str ());
	}
    }

  tdep->type_map.emplace (type_name, flags_type);
  return flags_type;
}

static struct type *
gdb_type_from_type_name (struct gdbarch *gdbarch, const std::string &type_name)
{
  size_t pos;

  /* vector types.  */
  if ((pos = type_name.find_last_of ('[')) != std::string::npos)
    {
      amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);

      auto it = tdep->type_map.find (type_name);
      if (it != tdep->type_map.end ())
	return it->second;

      struct type *vector_type
	= init_vector_type (gdb_type_from_type_name (gdbarch,
						     type_name.substr (0,
								       pos)),
			    std::stoi (type_name.substr (pos + 1)));

      vector_type->set_name (
	tdep->type_map.emplace (type_name, vector_type).first->first.c_str ());

      return vector_type;
    }
  /* scalar types.  */
  else if (type_name == "int32_t")
    return builtin_type (gdbarch)->builtin_int32;
  else if (type_name == "uint32_t")
    return builtin_type (gdbarch)->builtin_uint32;
  else if (type_name == "int64_t")
    return builtin_type (gdbarch)->builtin_int64;
  else if (type_name == "uint64_t")
    return builtin_type (gdbarch)->builtin_uint64;
  else if (type_name == "float")
    return builtin_type (gdbarch)->builtin_float;
  else if (type_name == "double")
    return builtin_type (gdbarch)->builtin_double;
  else if (type_name == "void (*)()")
    return builtin_type (gdbarch)->builtin_func_ptr;
  else if (type_name.find ("flags32_t") == 0
	   || type_name.find ("flags64_t") == 0)
    {
      std::regex regex (
	"(flags32_t|flags64_t)\\s+(\\w+)\\s*(\\{\\s*(.*)\\})?");

      /* Split 'type_name' into 3 tokens: "(type) (name) { (fields) }".  */
      std::sregex_token_iterator iter (type_name.begin (), type_name.end (),
				       regex, { 1, 2, 4 });
      std::vector<std::string> tokens (iter, std::sregex_token_iterator ());

      if (tokens.size () == 3)
	return amdgcn_flags_type (gdbarch, tokens[0] == "flags32_t" ? 32 : 64,
				  "builtin_type_amdgcn_flags_" + tokens[1],
				  tokens[2]);
    }
  else if (type_name.find ("enum") == 0)
    {
      std::regex regex ("enum\\s+(\\w+)\\s*(\\{\\s*(.*)\\})?");

      /* Split 'type_name' into 2 tokens: "(name) { (fields) }".  */
      std::sregex_token_iterator iter (type_name.begin (), type_name.end (),
				       regex, { 1, 3 });
      std::vector<std::string> tokens (iter, std::sregex_token_iterator ());

      if (tokens.size () == 2)
	return amdgcn_enum_type (gdbarch, 32 /* enum is implicitly a uint  */,
				 "builtin_type_amdgcn_enum_" + tokens[0],
				 tokens[1]);
    }

  return builtin_type (gdbarch)->builtin_void;
}

static struct type *
amdgcn_register_type (struct gdbarch *gdbarch, int regnum)
{
  amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);
  char *bytes;

  if (amd_dbgapi_register_get_info (tdep->register_ids[regnum],
				    AMD_DBGAPI_REGISTER_INFO_TYPE,
				    sizeof (bytes), &bytes)
      == AMD_DBGAPI_STATUS_SUCCESS)
    {
      std::string type_name (bytes);
      xfree (bytes);

      return gdb_type_from_type_name (gdbarch, type_name);
    }

  return builtin_type (gdbarch)->builtin_void;
}

static int
amdgcn_register_reggroup_p (struct gdbarch *gdbarch, int regnum,
			    struct reggroup *group)
{
  amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);
  const char *name = reggroup_name (group);

  auto it = tdep->register_class_map.find (name);
  if (it == tdep->register_class_map.end ())
    return group == all_reggroup;

  amd_dbgapi_register_class_state_t state;

  if (amd_dbgapi_register_is_in_register_class (it->second,
						tdep->register_ids[regnum],
						&state)
      != AMD_DBGAPI_STATUS_SUCCESS)
    return group == all_reggroup;

  return state == AMD_DBGAPI_REGISTER_CLASS_STATE_MEMBER
	 || group == all_reggroup;
}

static int
amdgcn_breakpoint_kind_from_pc (struct gdbarch *gdbarch, CORE_ADDR *)
{
  return get_amdgcn_gdbarch_tdep (gdbarch)->breakpoint_instruction_size;
}

static const gdb_byte *
amdgcn_sw_breakpoint_from_kind (struct gdbarch *gdbarch, int kind, int *size)
{
  *size = kind;
  return get_amdgcn_gdbarch_tdep (gdbarch)->breakpoint_instruction_bytes.get ();
}

struct amdgcn_frame_cache
{
  CORE_ADDR base;
  CORE_ADDR pc;
};

static struct amdgcn_frame_cache *
amdgcn_frame_cache (struct frame_info *this_frame, void **this_cache)
{
  if (*this_cache)
    return (struct amdgcn_frame_cache *) *this_cache;

  struct amdgcn_frame_cache *cache
    = FRAME_OBSTACK_ZALLOC (struct amdgcn_frame_cache);
  (*this_cache) = cache;

  cache->pc = get_frame_func (this_frame);
  cache->base = 0;

  return cache;
}

static void
amdgcn_frame_this_id (struct frame_info *this_frame, void **this_cache,
		      struct frame_id *this_id)
{
  struct amdgcn_frame_cache *cache
    = amdgcn_frame_cache (this_frame, this_cache);

  if (get_frame_type (this_frame) == INLINE_FRAME)
    (*this_id) = frame_id_build (cache->base, cache->pc);
  else
    (*this_id) = outer_frame_id;

  if (frame_debug)
    {
      fprintf_unfiltered (gdb_stdlog,
			  "{ amdgcn_frame_this_id (this_frame=%d) type=%d"
			  " -> %s }\n",
			  frame_relative_level (this_frame),
			  get_frame_type (this_frame),
			  this_id->to_string ().c_str ());
    }

  return;
}

static struct frame_id
amdgcn_dummy_id (struct gdbarch *gdbarch, struct frame_info *this_frame)
{
  return frame_id_build (0, get_frame_pc (this_frame));
}

static struct value *
amdgcn_frame_prev_register (struct frame_info *this_frame, void **this_cache,
			    int regnum)
{
  return frame_unwind_got_register (this_frame, regnum, regnum);
}

static const struct frame_unwind amdgcn_frame_unwind = {
  "amdgpu prologue",
  NORMAL_FRAME,
  default_frame_unwind_stop_reason,
  amdgcn_frame_this_id,
  amdgcn_frame_prev_register,
  NULL,
  default_frame_sniffer,
  NULL,
  NULL,
};

static int
print_insn_amdgcn (bfd_vma memaddr, struct disassemble_info *info)
{
  gdb_disassembler *di
    = static_cast<gdb_disassembler *> (info->application_data);

  /* Try to read at most instruction_size bytes.  */

  amd_dbgapi_size_t instruction_size = gdbarch_max_insn_length (di->arch ());
  gdb::unique_xmalloc_ptr<gdb_byte> buffer (
    (gdb_byte *) xmalloc (instruction_size));

  /* read_memory_func doesn't support partial reads, so if the read
     fails, try one byte less, on and on until we manage to read
     something.  A case where this would happen is if we're trying to
     read the last instruction at the end of a file section and that
     instruction is smaller than the largest instruction.  */
  while (instruction_size > 0)
    {
      if (info->read_memory_func (memaddr, buffer.get (), instruction_size,
				  info)
	  == 0)
	break;
      --instruction_size;
    }
  if (instruction_size == 0)
    {
      info->memory_error_func (-1, memaddr, info);
      return -1;
    }

  amd_dbgapi_architecture_id_t architecture_id;
  if (amd_dbgapi_get_architecture (gdbarch_bfd_arch_info (di->arch ())->mach,
				   &architecture_id)
      != AMD_DBGAPI_STATUS_SUCCESS)
    return -1;

  auto symbolizer = [] (amd_dbgapi_symbolizer_id_t symbolizer_id,
			amd_dbgapi_global_address_t address,
			char **symbol_text) -> amd_dbgapi_status_t
  {
    gdb_disassembler *disasm
      = reinterpret_cast<gdb_disassembler *> (symbolizer_id);

    string_file string (disasm->stream ()->can_emit_style_escape ());
    print_address (disasm->arch (), address, &string);
    *symbol_text = xstrdup (string.c_str ());

    return AMD_DBGAPI_STATUS_SUCCESS;
  };
  auto symbolizer_id = reinterpret_cast<amd_dbgapi_symbolizer_id_t> (di);

  char *instruction_text = nullptr;
  if (amd_dbgapi_disassemble_instruction (architecture_id, memaddr,
					  &instruction_size, buffer.get (),
					  &instruction_text, symbolizer_id,
					  symbolizer)
      != AMD_DBGAPI_STATUS_SUCCESS)
    {
      size_t alignment;
      if (amd_dbgapi_architecture_get_info (
	    architecture_id,
	    AMD_DBGAPI_ARCHITECTURE_INFO_MINIMUM_INSTRUCTION_ALIGNMENT,
	    sizeof (alignment), &alignment)
	  != AMD_DBGAPI_STATUS_SUCCESS)
	error (_ ("amd_dbgapi_architecture_get_info failed"));

      info->fprintf_func (info->stream, "<illegal instruction>");
      /* Skip to the next valid instruction address.  */
      return align_up (memaddr + 1, alignment) - memaddr;
    }

  /* Print the instruction.  */
  info->fprintf_func (info->stream, "%s", instruction_text);

  /* Free the memory allocated by the amd-dbgapi.  */
  xfree (instruction_text);

  return static_cast<int> (instruction_size);
}

/* Convert address space and segment address into a core address.  */
static CORE_ADDR
amdgcn_segment_address_to_core_address (arch_addr_space_id address_space_id,
					CORE_ADDR address)
{
  return (address & ~AMDGCN_ADDRESS_SPACE_MASK)
	 | (((CORE_ADDR) address_space_id) << AMDGCN_ADDRESS_SPACE_BIT_OFFSET);
}

/* Convert an integer to an address of a given segment address.  */
static CORE_ADDR
amdgcn_integer_to_address (struct gdbarch *gdbarch,
			   struct type *type, const gdb_byte *buf,
			   arch_addr_space_id address_space_id)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  CORE_ADDR segment_address
    = extract_signed_integer (buf, TYPE_LENGTH (type), byte_order);

  return amdgcn_segment_address_to_core_address (address_space_id,
						 segment_address);
}

/* See amdgcn-rocm-tdep.h.  */

arch_addr_space_id
amdgcn_address_space_id_from_core_address (CORE_ADDR addr)
{
  return (addr & AMDGCN_ADDRESS_SPACE_MASK) >> AMDGCN_ADDRESS_SPACE_BIT_OFFSET;
}

/* See amdgcn-rocm-tdep.h.  */

CORE_ADDR
amdgcn_segment_address_from_core_address (CORE_ADDR addr)
{
  return addr & ~AMDGCN_ADDRESS_SPACE_MASK;
}

/* Address class to address space mapping.

   TODO: This is just a quick fix to make the address class hand
	 written test work.

	 Unfortunately, there are only two bits currently in the type
	 instance flags used for address classes, and this is not
	 enough to describe the OpenCL address classes.

	 Another problem for even a basic compiler address class
	 support is the fact that both private_lane and private_wave
	 address spaces can be mapped to the same private address
	 class.

	 We will have to ignore that problem for now and assume that a
	 private address class is always mapped to a private_lane
	 address space.

	 This implementation will not be backward compatible when the
	 compiler starts generating the address class type information.  */

constexpr unsigned int DWARF_GLOBAL_ADDR_CLASS = 0;
constexpr unsigned int DWARF_GENERIC_ADDR_CLASS = 1;
constexpr unsigned int DWARF_LOCAL_ADDR_CLASS = 3;
constexpr unsigned int DWARF_PRIVATE_LANE_ADDR_CLASS = 5;

/* Map DWARF2_ADDR_CLASS address class to type instance flags.  */
static type_instance_flags
amdgcn_address_class_type_flags (int byte_size, int dwarf2_addr_class)
{
  if (dwarf2_addr_class == DWARF_GENERIC_ADDR_CLASS)
    return TYPE_INSTANCE_FLAG_ADDRESS_CLASS_1
	   | TYPE_INSTANCE_FLAG_ADDRESS_CLASS_2;
  else if (dwarf2_addr_class == DWARF_LOCAL_ADDR_CLASS)
    return TYPE_INSTANCE_FLAG_ADDRESS_CLASS_1;
  else if (dwarf2_addr_class == DWARF_PRIVATE_LANE_ADDR_CLASS)
    return TYPE_INSTANCE_FLAG_ADDRESS_CLASS_2;

  return 0;
}

/* Map TYPE_FLAGS type instance flags to address class.  */
static unsigned int
amdgcn_type_flags_to_addr_class (type_instance_flags type_flags)
{
  if ((type_flags & TYPE_INSTANCE_FLAG_ADDRESS_CLASS_1)
      && (type_flags & TYPE_INSTANCE_FLAG_ADDRESS_CLASS_2))
    return DWARF_GENERIC_ADDR_CLASS;
  else if (type_flags & TYPE_INSTANCE_FLAG_ADDRESS_CLASS_1)
    return DWARF_LOCAL_ADDR_CLASS;
  else if (type_flags & TYPE_INSTANCE_FLAG_ADDRESS_CLASS_2)
    return DWARF_PRIVATE_LANE_ADDR_CLASS;

  /* With current limitations, we can only assume
     that the address class is global.  */
  return DWARF_GLOBAL_ADDR_CLASS;
}

/* Map TYPE_FLAGS type instance flags to address class name.

   TODO: The idea of a target resolving a language address class name
	 just seems wrong.

	 At the moment, we assume that the language is OpenCL and
	 provide matching address class names.  */
static const char*
amdgcn_address_class_type_flags_to_name (struct gdbarch *gdbarch,
					 type_instance_flags type_flags)
{
  unsigned int addr_class = amdgcn_type_flags_to_addr_class (type_flags);

  if (addr_class == DWARF_GENERIC_ADDR_CLASS)
    return "generic";
  if (addr_class == DWARF_LOCAL_ADDR_CLASS)
    return "local";
  else if (addr_class == DWARF_PRIVATE_LANE_ADDR_CLASS)
    return "private";
  else
    return "";
}

/* Form a core address from a TYPE pointer type information and BUF
   pointer value buffer.

   TODO: The address class information belongs to a type, which means
	 that an address class is a language based concept, so either
	 the language should define the address class numbers and
	 names or some kind of a language enumeration needs to be
	 passed in.

	 At the moment, we assume that the language is OpenCL and we
	 apply 1-1 mapping between its address classes and rocm address
	 spaces.  */
static CORE_ADDR
amdgcn_pointer_to_address (struct gdbarch *gdbarch,
			   struct type *type, const gdb_byte *buf)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  CORE_ADDR address
    = extract_unsigned_integer (buf, TYPE_LENGTH (type), byte_order);
  unsigned int address_class
    = amdgcn_type_flags_to_addr_class (type->instance_flags ());

  /* Address might be in a converted format already, so even if the
     class is global, the address might have the address space part
     in it.  This happens in cases like 'p &local_array'."  */
  if (address_class == DWARF_GLOBAL_ADDR_CLASS)
    return address;

  /* In the current implementation, we shouldn't have a case where we
     have both type address class information as well as address
     space information in a core address.  */
  gdb_assert (!amdgcn_address_space_id_from_core_address (address));

  address = amdgcn_segment_address_from_core_address (address);

  return amdgcn_segment_address_to_core_address (address_class, address);
}

static CORE_ADDR
amdgcn_skip_prologue (struct gdbarch *gdbarch, CORE_ADDR start_pc)
{
  CORE_ADDR func_addr;

  /* See if we can determine the end of the prologue via the symbol table.
     If so, then return either PC, or the PC after the prologue, whichever
     is greater.  */
  if (find_pc_partial_function (start_pc, NULL, &func_addr, NULL))
    {
      CORE_ADDR post_prologue_pc
	= skip_prologue_using_sal (gdbarch, func_addr);
      struct compunit_symtab *cust = find_pc_compunit_symtab (func_addr);

      /* Clang always emits a line note before the prologue and another
	 one after.  We trust clang to emit usable line notes.  */
      if (post_prologue_pc
	  && (cust != NULL && COMPUNIT_PRODUCER (cust) != NULL
	      && producer_is_llvm (COMPUNIT_PRODUCER (cust))))
	return std::max (start_pc, post_prologue_pc);
    }

  /* Can't determine prologue from the symbol table, need to examine
     instructions.  */

  return start_pc;
}

static gdb::array_view<const arch_addr_space>
amdgcn_address_spaces (struct gdbarch *gdbarch)
{
  amdgcn_gdbarch_tdep *tdep = get_amdgcn_gdbarch_tdep (gdbarch);
  return tdep->address_spaces;
}

static simd_lanes_mask_t
amdgcn_rocm_active_lanes_mask (struct gdbarch *gdbarch, thread_info *tp)
{
  gdb_static_assert (sizeof (simd_lanes_mask_t) >= sizeof (uint64_t));

  uint64_t exec_mask;
  if (wave_get_info (tp, AMD_DBGAPI_WAVE_INFO_EXEC_MASK, exec_mask)
      != AMD_DBGAPI_STATUS_SUCCESS)
    return 0;

  return exec_mask;
}

static int
amdgcn_rocm_supported_lanes_count (struct gdbarch *gdbarch, thread_info *tp)
{
  size_t count;
  if (wave_get_info (tp, AMD_DBGAPI_WAVE_INFO_LANE_COUNT, count)
      != AMD_DBGAPI_STATUS_SUCCESS)
    return 0;

  return count;
}

/* A client debugger can check if the lane is part of a valid
   work-group by checking that the lane is in the range of the
   associated work-group within the grid, accounting for partial
   work-groups.  */

static int
amdgcn_rocm_used_lanes_count (struct gdbarch *gdbarch, thread_info *tp)
{
  amd_dbgapi_dispatch_id_t dispatch_id;
  if (wave_get_info (tp, AMD_DBGAPI_WAVE_INFO_DISPATCH, dispatch_id)
      != AMD_DBGAPI_STATUS_SUCCESS)
    {
      /* The dispatch associated with a wave is not available.  A wave
	 may not have an associated dispatch if attaching to a process
	 with already existing waves.  In that case, all we can do is
	 claim that all lanes are used.  */
      return amdgcn_rocm_supported_lanes_count (gdbarch, tp);
    }

  uint32_t grid_sizes[3];
  dispatch_get_info_throw (dispatch_id,
			   AMD_DBGAPI_DISPATCH_INFO_GRID_SIZES,
			   grid_sizes);

  uint16_t work_group_sizes[3];
  dispatch_get_info_throw (dispatch_id,
			   AMD_DBGAPI_DISPATCH_INFO_WORK_GROUP_SIZES,
			   work_group_sizes);

  uint32_t group_ids[3];
  wave_get_info_throw (tp, AMD_DBGAPI_WAVE_INFO_WORK_GROUP_COORD, group_ids);

  uint32_t wave_in_group;
  wave_get_info_throw (tp, AMD_DBGAPI_WAVE_INFO_WAVE_NUMBER_IN_WORK_GROUP,
		       wave_in_group);

  size_t lane_count;
  wave_get_info_throw (tp, AMD_DBGAPI_WAVE_INFO_LANE_COUNT, lane_count);

  size_t work_group_item_sizes[3];
  for (int i = 0; i < 3; i++)
    {
      size_t item_start = group_ids[i] * work_group_sizes[i];
      size_t item_end = item_start + work_group_sizes[i];
      if (item_end > grid_sizes[i])
	item_end = grid_sizes[i];
      work_group_item_sizes[i] = item_end - item_start;
    }

  size_t work_items = (work_group_item_sizes[0]
		       * work_group_item_sizes[1]
		       * work_group_item_sizes[2]);

  size_t work_items_left = work_items - wave_in_group * lane_count;
  return std::min (work_items_left, lane_count);
}

static struct gdbarch *
amdgcn_gdbarch_init (struct gdbarch_info info, struct gdbarch_list *arches)
{
  /* If there is already a candidate, use it.  */
  arches = gdbarch_list_lookup_by_info (arches, &info);
  if (arches != NULL)
    return arches->gdbarch;

  struct gdbarch_deleter
  {
    void
    operator() (struct gdbarch *gdbarch) const
    {
      gdbarch_free (gdbarch);
    }
  };

  /* Allocate space for the new architecture.  */
  std::unique_ptr<amdgcn_gdbarch_tdep> tdep (new amdgcn_gdbarch_tdep);
  std::unique_ptr<struct gdbarch, gdbarch_deleter> gdbarch_u (
    gdbarch_alloc (&info, tdep.get ()));

  struct gdbarch *gdbarch = gdbarch_u.get ();

  /* Data types.  */
  set_gdbarch_char_signed (gdbarch, 0);
  set_gdbarch_ptr_bit (gdbarch, 64);
  set_gdbarch_addr_bit (gdbarch, 64);
  set_gdbarch_short_bit (gdbarch, 16);
  set_gdbarch_int_bit (gdbarch, 32);
  set_gdbarch_long_bit (gdbarch, 64);
  set_gdbarch_long_long_bit (gdbarch, 64);
  set_gdbarch_float_bit (gdbarch, 32);
  set_gdbarch_double_bit (gdbarch, 64);
  set_gdbarch_long_double_bit (gdbarch, 128);
  set_gdbarch_float_format (gdbarch, floatformats_ieee_single);
  set_gdbarch_double_format (gdbarch, floatformats_ieee_double);
  set_gdbarch_long_double_format (gdbarch, floatformats_ieee_double);

/* Address space handling.  */
  set_gdbarch_integer_to_address (gdbarch, amdgcn_integer_to_address);
  set_gdbarch_address_space_id_from_core_address
    (gdbarch, amdgcn_address_space_id_from_core_address);
  set_gdbarch_segment_address_from_core_address
    (gdbarch, amdgcn_segment_address_from_core_address);
  set_gdbarch_segment_address_to_core_address
    (gdbarch, amdgcn_segment_address_to_core_address);
  set_gdbarch_address_spaces (gdbarch, amdgcn_address_spaces);

  /* Frame Interpretation.  */
  set_gdbarch_skip_prologue (gdbarch, amdgcn_skip_prologue);
  set_gdbarch_inner_than (gdbarch, core_addr_greaterthan);
  dwarf2_append_unwinders (gdbarch);
  frame_unwind_append_unwinder (gdbarch, &amdgcn_frame_unwind);
  set_gdbarch_dummy_id (gdbarch, amdgcn_dummy_id);

  set_gdbarch_pointer_to_address (gdbarch, amdgcn_pointer_to_address);
  set_gdbarch_address_class_type_flags
    (gdbarch, amdgcn_address_class_type_flags);
  set_gdbarch_address_class_type_flags_to_name
    (gdbarch, amdgcn_address_class_type_flags_to_name);

  /* Registers and Memory.  */
  amd_dbgapi_architecture_id_t architecture_id;
  if (amd_dbgapi_get_architecture (gdbarch_bfd_arch_info (gdbarch)->mach,
				   &architecture_id)
      != AMD_DBGAPI_STATUS_SUCCESS)
    return nullptr;

  size_t register_class_count;
  amd_dbgapi_register_class_id_t *register_class_ids;

  if (amd_dbgapi_architecture_register_class_list (architecture_id,
						   &register_class_count,
						   &register_class_ids)
      != AMD_DBGAPI_STATUS_SUCCESS)
    return nullptr;

  /* Add register groups.  */
  reggroup_add (gdbarch, all_reggroup);

  for (size_t i = 0; i < register_class_count; ++i)
    {
      char *bytes;
      if (amd_dbgapi_architecture_register_class_get_info (
	    register_class_ids[i], AMD_DBGAPI_REGISTER_CLASS_INFO_NAME,
	    sizeof (bytes), &bytes)
	  != AMD_DBGAPI_STATUS_SUCCESS)
	continue;

      gdb::unique_xmalloc_ptr<char> name (bytes);

      auto inserted = tdep->register_class_map.emplace (name.get (),
							register_class_ids[i]);

      if (!inserted.second)
	continue;

      /* Allocate the reggroup in the gdbarch.  */
      auto *group = reggroup_gdbarch_new (gdbarch, name.get (), USER_REGGROUP);
      if (!group)
	{
	  tdep->register_class_map.erase (inserted.first);
	  continue;
	}

      reggroup_add (gdbarch, group);
    }
  xfree (register_class_ids);

  /* Add registers. */
  size_t register_count;
  amd_dbgapi_register_id_t *register_ids;

  if (amd_dbgapi_architecture_register_list (architecture_id, &register_count,
					     &register_ids)
      != AMD_DBGAPI_STATUS_SUCCESS)
    return nullptr;

  gdb::unique_xmalloc_ptr<amd_dbgapi_register_id_t> register_ids_holder (
    register_ids);

  tdep->register_ids.insert (tdep->register_ids.end (), &register_ids[0],
			     &register_ids[register_count]);

  set_gdbarch_num_regs (gdbarch, register_count);
  set_gdbarch_num_pseudo_regs (gdbarch, 0);

  tdep->register_names.resize (register_count);
  for (size_t i = 0; i < register_count; ++i)
    {
      if (!tdep->regnum_map.emplace (tdep->register_ids[i], i).second)
	return nullptr;

      /* Get register name.  */
      char *bytes;
      if (amd_dbgapi_register_get_info (tdep->register_ids[i],
					AMD_DBGAPI_REGISTER_INFO_NAME,
					sizeof (bytes), &bytes)
	  == AMD_DBGAPI_STATUS_SUCCESS)
	{
	  tdep->register_names[i] = bytes;
	  xfree (bytes);
	}

      /* Get register DWARF number.  */
      uint64_t dwarf_num;
      if (amd_dbgapi_register_get_info (tdep->register_ids[i],
					AMD_DBGAPI_REGISTER_INFO_DWARF,
					sizeof (dwarf_num), &dwarf_num)
	  == AMD_DBGAPI_STATUS_SUCCESS)
	{
	  if (dwarf_num >= tdep->dwarf_regnum_to_gdb_regnum.size ())
	    tdep->dwarf_regnum_to_gdb_regnum.resize (dwarf_num + 1, -1);

	  tdep->dwarf_regnum_to_gdb_regnum[dwarf_num] = i;
	}
    }

  amd_dbgapi_register_id_t pc_register_id;
  if (
    amd_dbgapi_architecture_get_info (architecture_id,
				      AMD_DBGAPI_ARCHITECTURE_INFO_PC_REGISTER,
				      sizeof (pc_register_id), &pc_register_id)
    != AMD_DBGAPI_STATUS_SUCCESS)
    return nullptr;

  set_gdbarch_pc_regnum (gdbarch, tdep->regnum_map[pc_register_id]);
  set_gdbarch_ps_regnum (gdbarch, -1);
  set_gdbarch_sp_regnum (gdbarch, -1);
  set_gdbarch_fp0_regnum (gdbarch, -1);

  set_gdbarch_dwarf2_reg_to_regnum (gdbarch, amdgcn_dwarf_reg_to_regnum);

  set_gdbarch_return_value (gdbarch, amdgcn_return_value);

  /* Register Representation.  */
  set_gdbarch_register_name (gdbarch, amdgcn_register_name);
  set_gdbarch_register_type (gdbarch, amdgcn_register_type);
  set_gdbarch_register_reggroup_p (gdbarch, amdgcn_register_reggroup_p);

  /* Disassembly.  */
  set_gdbarch_print_insn (gdbarch, print_insn_amdgcn);

  /* Instructions.  */
  amd_dbgapi_size_t max_insn_length = 0;
  if (amd_dbgapi_architecture_get_info (
	architecture_id, AMD_DBGAPI_ARCHITECTURE_INFO_LARGEST_INSTRUCTION_SIZE,
	sizeof (max_insn_length), &max_insn_length)
      != AMD_DBGAPI_STATUS_SUCCESS)
    error (_ ("amd_dbgapi_architecture_get_info failed"));

  set_gdbarch_max_insn_length (gdbarch, max_insn_length);

  /* Lane debugging.  */
  set_gdbarch_active_lanes_mask (gdbarch, amdgcn_rocm_active_lanes_mask);
  set_gdbarch_supported_lanes_count (gdbarch, amdgcn_rocm_supported_lanes_count);
  set_gdbarch_used_lanes_count (gdbarch, amdgcn_rocm_used_lanes_count);

  if (amd_dbgapi_architecture_get_info (
	architecture_id,
	AMD_DBGAPI_ARCHITECTURE_INFO_BREAKPOINT_INSTRUCTION_SIZE,
	sizeof (tdep->breakpoint_instruction_size),
	&tdep->breakpoint_instruction_size)
      != AMD_DBGAPI_STATUS_SUCCESS)
    error (_ ("amd_dbgapi_architecture_get_info failed"));

  gdb_byte *breakpoint_instruction_bytes;
  if (amd_dbgapi_architecture_get_info (
	architecture_id, AMD_DBGAPI_ARCHITECTURE_INFO_BREAKPOINT_INSTRUCTION,
	sizeof (breakpoint_instruction_bytes), &breakpoint_instruction_bytes)
      != AMD_DBGAPI_STATUS_SUCCESS)
    error (_ ("amd_dbgapi_architecture_get_info failed"));

  tdep->breakpoint_instruction_bytes.reset (breakpoint_instruction_bytes);

  set_gdbarch_breakpoint_kind_from_pc (gdbarch,
				       amdgcn_breakpoint_kind_from_pc);
  set_gdbarch_sw_breakpoint_from_kind (gdbarch,
				       amdgcn_sw_breakpoint_from_kind);

  amd_dbgapi_size_t pc_adjust;
  if (amd_dbgapi_architecture_get_info (
	architecture_id,
	AMD_DBGAPI_ARCHITECTURE_INFO_BREAKPOINT_INSTRUCTION_PC_ADJUST,
	sizeof (pc_adjust), &pc_adjust)
      != AMD_DBGAPI_STATUS_SUCCESS)
    error (_ ("amd_dbgapi_architecture_get_info failed"));

  set_gdbarch_decr_pc_after_break (gdbarch, pc_adjust);

  /* Get info about address spaces.  */
  size_t address_space_count;
  amd_dbgapi_address_space_id_t *address_spaces;

  if (amd_dbgapi_architecture_address_space_list (architecture_id,
						  &address_space_count,
						  &address_spaces)
      != AMD_DBGAPI_STATUS_SUCCESS)
    error (_("amd_dbgapi_architecture_address_space_list failed"));

  gdb::unique_xmalloc_ptr<amd_dbgapi_address_space_id_t[]> address_spaces_holder
    (address_spaces);

  for (size_t i = 0; i < address_space_count; ++i)
    {
      char *address_space_name;

      if (amd_dbgapi_address_space_get_info
	    (address_spaces[i], AMD_DBGAPI_ADDRESS_SPACE_INFO_NAME,
	     sizeof (address_space_name), &address_space_name)
	  != AMD_DBGAPI_STATUS_SUCCESS)
	error (_("amd_dbgapi_address_space_get_info (name) failed"));

      gdb_assert (address_space_name != nullptr);
      gdb::unique_xmalloc_ptr<char> address_space_name_holder
	(address_space_name);

      arch_addr_space_id address_space_dwarf_num;
      if (amd_dbgapi_address_space_get_info
	    (address_spaces[i], AMD_DBGAPI_ADDRESS_SPACE_INFO_DWARF,
	     sizeof (address_space_dwarf_num), &address_space_dwarf_num)
	  != AMD_DBGAPI_STATUS_SUCCESS)
	error (_("amd_dbgapi_address_space_get_info (dwarf) failed"));

      tdep->address_spaces.emplace_back (address_space_dwarf_num,
					 std::move (address_space_name_holder));
    }

  tdep.release ();
  gdbarch_u.release ();

  return gdbarch;
}

/* Provide a prototype to silence -Wmissing-prototypes.  */
extern initialize_file_ftype _initialize_amdgcn_rocm_tdep;

void
_initialize_amdgcn_rocm_tdep ()
{
  gdbarch_register (bfd_arch_amdgcn, amdgcn_gdbarch_init, NULL);
}
