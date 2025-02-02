/* Displaced stepping related things.

   Copyright (C) 2020-2021 Free Software Foundation, Inc.
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
#include "displaced-stepping.h"

#include "cli/cli-cmds.h"
#include "command.h"
#include "gdbarch.h"
#include "gdbcore.h"
#include "gdbthread.h"
#include "inferior.h"
#include "regcache.h"
#include "target/target.h"

/* Default destructor for displaced_step_copy_insn_closure.  */

displaced_step_copy_insn_closure::~displaced_step_copy_insn_closure ()
  = default;

bool debug_displaced = false;

static void
show_debug_displaced (struct ui_file *file, int from_tty,
		      struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("Displace stepping debugging is %s.\n"), value);
}

displaced_step_prepare_status
displaced_step_buffers::prepare (thread_info *thread, CORE_ADDR &displaced_pc)
{
  gdb_assert (!thread->displaced_step_state.in_progress ());

  /* Sanity check: the thread should not be using a buffer at this point.  */
  for (displaced_step_buffer &buf : m_buffers)
    gdb_assert (buf.current_thread != thread);

  regcache *regcache = get_thread_regcache (thread);
  const address_space *aspace = regcache->aspace ();
  gdbarch *arch = regcache->arch ();
  ULONGEST len = gdbarch_max_insn_length (arch);

  /* Search for an unused buffer.  */
  displaced_step_buffer *buffer = nullptr;
  displaced_step_prepare_status fail_status
    = DISPLACED_STEP_PREPARE_STATUS_CANT;

  for (displaced_step_buffer &candidate : m_buffers)
    {
      bool bp_in_range = breakpoint_in_range_p (aspace, candidate.addr, len);
      bool is_free = candidate.current_thread == nullptr;

      if (!bp_in_range)
	{
	  if (is_free)
	    {
	      buffer = &candidate;
	      break;
	    }
	  else
	    {
	      /* This buffer would be suitable, but it's used right now.  */
	      fail_status = DISPLACED_STEP_PREPARE_STATUS_UNAVAILABLE;
	    }
	}
      else
	{
	  /* There's a breakpoint set in the scratch pad location range
	     (which is usually around the entry point).  We'd either
	     install it before resuming, which would overwrite/corrupt the
	     scratch pad, or if it was already inserted, this displaced
	     step would overwrite it.  The latter is OK in the sense that
	     we already assume that no thread is going to execute the code
	     in the scratch pad range (after initial startup) anyway, but
	     the former is unacceptable.  Simply punt and fallback to
	     stepping over this breakpoint in-line.  */
	  displaced_debug_printf ("breakpoint set in displaced stepping "
				  "buffer at %s, can't use.",
				  paddress (arch, candidate.addr));
	}
    }

  if (buffer == nullptr)
    return fail_status;

  displaced_debug_printf ("selected buffer at %s",
			  paddress (arch, buffer->addr));

  /* Save the original PC of the thread.  */
  buffer->original_pc = regcache_read_pc (regcache);

  /* Return displaced step buffer address to caller.  */
  displaced_pc = buffer->addr;

  /* Save the original contents of the displaced stepping buffer.  */
  buffer->saved_copy.resize (len);

  int status = target_read_memory (buffer->addr,
				    buffer->saved_copy.data (), len);
  if (status != 0)
    throw_error (MEMORY_ERROR,
		 _("Error accessing memory address %s (%s) for "
		   "displaced-stepping scratch space."),
		 paddress (arch, buffer->addr), safe_strerror (status));

  displaced_debug_printf ("saved %s: %s",
			  paddress (arch, buffer->addr),
			  displaced_step_dump_bytes
			  (buffer->saved_copy.data (), len).c_str ());

  /* Save this in a local variable first, so it's released if code below
     throws.  */
  displaced_step_copy_insn_closure_up copy_insn_closure
    = gdbarch_displaced_step_copy_insn (arch, buffer->original_pc,
					buffer->addr, regcache);

  if (copy_insn_closure == nullptr)
    {
      /* The architecture doesn't know how or want to displaced step
	 this instruction or instruction sequence.  Fallback to
	 stepping over the breakpoint in-line.  */
      return DISPLACED_STEP_PREPARE_STATUS_CANT;
    }

  /* Resume execution at the copy.  */
  regcache_write_pc (regcache, buffer->addr);

  /* This marks the buffer as being in use.  */
  buffer->current_thread = thread;

  /* Save this, now that we know everything went fine.  */
  buffer->copy_insn_closure = std::move (copy_insn_closure);

  /* Tell infrun not to try preparing a displaced step again for this inferior if
     all buffers are taken.  */
  thread->inf->displaced_step_state.unavailable = true;
  for (const displaced_step_buffer &buf : m_buffers)
    {
      if (buf.current_thread == nullptr)
	{
	  thread->inf->displaced_step_state.unavailable = false;
	  break;
	}
    }

  return DISPLACED_STEP_PREPARE_STATUS_OK;
}

static void
write_memory_ptid (ptid_t ptid, CORE_ADDR memaddr,
		   const gdb_byte *myaddr, int len)
{
  scoped_restore save_inferior_ptid = make_scoped_restore (&inferior_ptid);

  inferior_ptid = ptid;
  write_memory (memaddr, myaddr, len);
}

static bool
displaced_step_instruction_executed_successfully (gdbarch *arch,
						  gdb_signal signal)
{
  if (signal != GDB_SIGNAL_TRAP)
    return false;

  if (target_stopped_by_watchpoint ())
    {
      if (gdbarch_have_nonsteppable_watchpoint (arch)
	  || target_have_steppable_watchpoint ())
	return false;
    }

  return true;
}

displaced_step_finish_status
displaced_step_buffers::finish (gdbarch *arch, thread_info *thread,
				gdb_signal sig)
{
  gdb_assert (thread->displaced_step_state.in_progress ());

  /* Find the buffer this thread was using.  */
  displaced_step_buffer *buffer = nullptr;

  for (displaced_step_buffer &candidate : m_buffers)
    {
      if (candidate.current_thread == thread)
	{
	  buffer = &candidate;
	  break;
	}
    }

  gdb_assert (buffer != nullptr);

  /* Move this to a local variable so it's released in case something goes
     wrong.  */
  displaced_step_copy_insn_closure_up copy_insn_closure
    = std::move (buffer->copy_insn_closure);
  gdb_assert (copy_insn_closure != nullptr);

  /* Reset BUFFER->CURRENT_THREAD immediately to mark the buffer as available,
     in case something goes wrong below.  */
  buffer->current_thread = nullptr;

  /* Now that a buffer gets freed, tell infrun it can ask us to prepare a displaced
     step again for this inferior.  Do that here in case something goes wrong
     below.  */
  thread->inf->displaced_step_state.unavailable = false;

  ULONGEST len = gdbarch_max_insn_length (arch);

  /* Restore memory of the buffer.  */
  write_memory_ptid (thread->ptid, buffer->addr,
		     buffer->saved_copy.data (), len);

  displaced_debug_printf ("restored %s %s",
			  target_pid_to_str (thread->ptid).c_str (),
			  paddress (arch, buffer->addr));

  regcache *rc = get_thread_regcache (thread);

  bool instruction_executed_successfully
    = displaced_step_instruction_executed_successfully (arch, sig);

  if (instruction_executed_successfully)
    {
      gdbarch_displaced_step_fixup (arch, copy_insn_closure.get (),
				    buffer->original_pc,
				    buffer->addr, rc);
      return DISPLACED_STEP_FINISH_STATUS_OK;
    }
  else
    {
      /* Since the instruction didn't complete, all we can do is relocate the
	 PC.  */
      CORE_ADDR pc = regcache_read_pc (rc);
      pc = buffer->original_pc + (pc - buffer->addr);
      regcache_write_pc (rc, pc);
      return DISPLACED_STEP_FINISH_STATUS_NOT_EXECUTED;
    }
}

const displaced_step_copy_insn_closure *
displaced_step_buffers::copy_insn_closure_by_addr (CORE_ADDR addr)
{
  for (const displaced_step_buffer &buffer : m_buffers)
    {
      if (addr == buffer.addr)
	return buffer.copy_insn_closure.get ();
    }

  return nullptr;
}

void
displaced_step_buffers::restore_in_ptid (ptid_t ptid)
{
  for (const displaced_step_buffer &buffer : m_buffers)
    {
      if (buffer.current_thread == nullptr)
	continue;

      regcache *regcache = get_thread_regcache (buffer.current_thread);
      gdbarch *arch = regcache->arch ();
      ULONGEST len = gdbarch_max_insn_length (arch);

      write_memory_ptid (ptid, buffer.addr, buffer.saved_copy.data (), len);

      displaced_debug_printf ("restored in ptid %s %s",
			      target_pid_to_str (ptid).c_str (),
			      paddress (arch, buffer.addr));
    }
}

void _initialize_displaced_stepping ();
void
_initialize_displaced_stepping ()
{
  add_setshow_boolean_cmd ("displaced", class_maintenance,
			   &debug_displaced, _("\
Set displaced stepping debugging."), _("\
Show displaced stepping debugging."), _("\
When non-zero, displaced stepping specific debugging is enabled."),
			    NULL,
			    show_debug_displaced,
			    &setdebuglist, &showdebuglist);
}

bool
default_supports_displaced_step (target_ops *target, thread_info *thread)
{
  /* Only check for the presence of `prepare`.  The gdbarch verification ensures
     that if `prepare` is provided, so is `finish`.  */
  gdbarch *arch = get_thread_regcache (thread)->arch ();
  return gdbarch_displaced_step_prepare_p (arch);
}

displaced_step_prepare_status
default_displaced_step_prepare (target_ops *target, thread_info *thread,
				CORE_ADDR &displaced_pc)
{
  gdbarch *arch = get_thread_regcache (thread)->arch ();
  return gdbarch_displaced_step_prepare (arch, thread, displaced_pc);
}

displaced_step_finish_status
default_displaced_step_finish (target_ops *target,
			       thread_info *thread,
			       gdb_signal sig)
{
  gdbarch *arch = get_thread_regcache (thread)->arch ();
  return gdbarch_displaced_step_finish (arch, thread, sig);
}
