// ACME - a crossassembler for producing 6502/65c02/65816/65ce02 code.
// Copyright (C) 1998-2020 Marco Baye
// Have a look at "acme.c" for further info
//
// Output stuff
// 24 Nov 2007	Added possibility to suppress segment overlap warnings
// 25 Sep 2011	Fixed bug in !to (colons in filename could be interpreted as EOS)
//  5 Mar 2014	Fixed bug where setting *>0xffff resulted in hangups.
// 19 Nov 2014	Merged Johann Klasek's report listing generator patch
// 22 Sep 2015	Added big-endian output functions
// 20 Apr 2019	Prepared for "make segment overlap warnings into errors" later on
#include "output.h"
#include <stdlib.h>
#include <string.h>	// for memset()
#include "acme.h"
#include "alu.h"
#include "config.h"
#include "cpu.h"
#include "dynabuf.h"
#include "global.h"
#include "input.h"
#include "platform.h"
#include "tree.h"


// constants
#define NO_SEGMENT_START	(-1)	// invalid value to signal "not in a segment"


// structure for linked list of segment data
struct segment {
	struct segment	*next,
			*prev;
	intval_t	start,
			length;
};

// structure for all output stuff:
struct output {
	// output buffer stuff
	intval_t	bufsize;	// either 64 KiB or 16 MiB
	char		*buffer;	// holds assembled code
	intval_t	write_idx;	// index of next write
	intval_t	lowest_written;		// smallest address used
	intval_t	highest_written;	// largest address used
	boolean		initvalue_set;
	char      fill_value;
	struct {
		intval_t	start;	// start of current segment (or NO_SEGMENT_START)
		intval_t	max;	// highest address segment may use
		bits		flags;	// segment flags ("overlay" and "invisible", see header file)
		struct segment	list_head;	// head element of doubly-linked ring list
	} segment;
	char		xor;		// output modifier
};

// for offset assembly:
static struct pseudopc	*pseudopc_current_context;	// current struct (NULL when not in pseudopc block)


// variables
static struct output	default_output;
static struct output	*out	= &default_output;
// FIXME - make static
struct vcpu		CPU_state;	// current CPU state

// FIXME - move output _file_ stuff to some other .c file!
// possible file formats
enum output_format {
	OUTPUT_FORMAT_UNSPECIFIED,	// default (uses "plain" actually)
	OUTPUT_FORMAT_APPLE,		// load address, length, code
	OUTPUT_FORMAT_CBM,		// load address, code (default for "!to" pseudo opcode)
	OUTPUT_FORMAT_PLAIN,		// code only
	OUTPUT_FORMAT_HEX
};
// predefined stuff
// tree to hold output formats (FIXME - a tree for three items, really?)
static struct ronode	file_format_tree[]	= {
	PREDEF_START,
#define KNOWN_FORMATS	"'plain', 'cbm', 'apple', 'hex'"	// shown in CLI error message for unknown formats
	PREDEFNODE("apple",	OUTPUT_FORMAT_APPLE),
	PREDEFNODE("cbm",	OUTPUT_FORMAT_CBM),
//	PREDEFNODE("o65",	OUTPUT_FORMAT_O65),
	PREDEFNODE("plain",	OUTPUT_FORMAT_PLAIN),
	PREDEF_END("hex",	OUTPUT_FORMAT_HEX),
	//    ^^^^ this marks the last element
};
// chosen file format
static enum output_format	output_format	= OUTPUT_FORMAT_UNSPECIFIED;
const char			outputfile_formats[]	= KNOWN_FORMATS;	// string to show if outputfile_set_format() returns nonzero


// report binary output
static void report_binary(char value)
{
	if (report->bin_used == 0)
		report->bin_address = out->write_idx;	// remember address at start of line
	if (report->bin_used < REPORT_BINBUFSIZE)
		report->bin_buf[report->bin_used++] = value;
}


// set up new out->segment.max value according to the given address.
// just find the next segment start and subtract 1.
static void find_segment_max(intval_t new_pc)
{
	struct segment	*test_segment	= out->segment.list_head.next;

	// search for smallest segment start address that
	// is larger than given address
	// use list head as sentinel
// FIXME - if +1 overflows intval_t, we have an infinite loop!
	out->segment.list_head.start = new_pc + 1;
	while (test_segment->start <= new_pc)
		test_segment = test_segment->next;
	if (test_segment == &out->segment.list_head)
		out->segment.max = out->bufsize - 1;
	else
		out->segment.max = test_segment->start - 1;	// last free address available
}


//
static void border_crossed(int current_offset)
{
	if (current_offset >= out->bufsize)
		Throw_serious_error("Produced too much code.");
	// TODO - get rid of FIRST_PASS condition, because user can suppress these warnings if they want
	if (FIRST_PASS) {
		// TODO: make warn/err an arg for a general "Throw" function
		if (config.segment_warning_is_error)
			Throw_error("Segment reached another one, overwriting it.");
		else
			Throw_warning("Segment reached another one, overwriting it.");
		find_segment_max(current_offset + 1);	// find new (next) limit
	}
}


// function ptr to write byte into output buffer (might point to real fn or error trigger)
void (*Output_byte)(intval_t byte);


// send low byte to output buffer, automatically increasing program counter
static void real_output(intval_t byte)
{
	// CAUTION - there are two copies of these checks!
	// TODO - add additional check for current segment's "limit" value
	// did we reach next segment?
	if (out->write_idx > out->segment.max)
		border_crossed(out->write_idx);
	// new minimum address?
	if (out->write_idx < out->lowest_written)
		out->lowest_written = out->write_idx;
	// new maximum address?
	if (out->write_idx > out->highest_written)
		out->highest_written = out->write_idx;
	// write byte and advance ptrs
	if (report->fd)
		report_binary(byte & 0xff);	// file for reporting, taking also CPU_2add
	out->buffer[out->write_idx++] = (byte & 0xff) ^ out->xor;
	++CPU_state.add_to_pc;
}


// throw error (pc undefined) and use fake pc from now on
static void no_output(intval_t byte)
{
	Throw_error(exception_pc_undefined);
	// now change fn ptr to not complain again.
	Output_byte = real_output;
	Output_byte(byte);	// try again
}


// skip over some bytes in output buffer without starting a new segment
// (used by "!skip", and also called by "!binary" if really calling
// Output_byte would be a waste of time)
void output_skip(int size)
{
	if (size < 1) {
		// FIXME - ok for zero, but why is there no error message
		// output for negative values?
		return;
	}

	// check whether ptr undefined
	if (Output_byte == no_output) {
		Output_byte(0);	// trigger error with a dummy byte
		--size;	// fix amount to cater for dummy byte
	}
	// CAUTION - there are two copies of these checks!
	// TODO - add additional check for current segment's "limit" value
	// did we reach next segment?
	if (out->write_idx + size - 1 > out->segment.max)
		border_crossed(out->write_idx + size - 1);
	// new minimum address?
	if (out->write_idx < out->lowest_written)
		out->lowest_written = out->write_idx;
	// new maximum address?
	if (out->write_idx + size - 1 > out->highest_written)
		out->highest_written = out->write_idx + size - 1;
	// advance ptrs
	out->write_idx += size;
	CPU_state.add_to_pc += size;
}


// fill output buffer with given byte value
static void fill_completely(char value)
{
	memset(out->buffer, value, out->bufsize);
	out->fill_value = value;
}


// define default value for empty memory ("!initmem" pseudo opcode)
// returns zero if ok, nonzero if already set
int output_initmem(char content)
{
	// if MemInit flag is already set, complain
	if (out->initvalue_set) {
		Throw_warning("Memory already initialised.");
		return 1;	// failed
	}
	// set MemInit flag
	out->initvalue_set = TRUE;
	// init memory
	fill_completely(content);
	// enforce another pass
	if (pass.undefined_count == 0)
		pass.undefined_count = 1;
	//if (pass.needvalue_count == 0)	FIXME - use? instead or additionally?
	//	pass.needvalue_count = 1;
// FIXME - enforcing another pass is not needed if there hasn't been any
// output yet. But that's tricky to detect without too much overhead.
// The old solution was to add &&(out->lowest_written < out->highest_written+1) to "if" above
	return 0;	// ok
}


// try to set output format held in DynaBuf. Returns zero on success.
int outputfile_set_format(void)
{
	void	*node_body;

	// perform lookup
	if (!Tree_easy_scan(file_format_tree, &node_body, GlobalDynaBuf))
		return 1;

	output_format = (enum output_format) node_body;
	return 0;
}

// if file format was already chosen, returns zero.
// if file format isn't set, chooses CBM and returns 1.
int outputfile_prefer_cbm_format(void)
{
	if (output_format != OUTPUT_FORMAT_UNSPECIFIED)
		return 0;

	output_format = OUTPUT_FORMAT_CBM;
	return 1;
}

// select output file ("!to" pseudo opcode)
// returns zero on success, nonzero if already set
int outputfile_set_filename(void)
{
	// if output file already chosen, complain and exit
	if (output_filename) {
		Throw_warning("Output file already chosen.");
		return 1;	// failed
	}

	// get malloc'd copy of filename
	output_filename = DynaBuf_get_copy(GlobalDynaBuf);
	return 0;	// ok
}


// init output struct (done later)
void Output_init(signed long fill_value, boolean use_large_buf)
{
	out->bufsize = use_large_buf ? 0x1000000 : 0x10000;
	out->buffer = safe_malloc(out->bufsize);
	if (fill_value == MEMINIT_USE_DEFAULT) {
		fill_value = FILLVALUE_INITIAL;
		out->initvalue_set = FALSE;
	} else {
		out->initvalue_set = TRUE;
	}
	// init output buffer (fill memory with initial value)
	fill_completely(fill_value & 0xff);

	// init ring list of segments
	out->segment.list_head.next = &out->segment.list_head;
	out->segment.list_head.prev = &out->segment.list_head;
}

// output a single line in Intel HEX format
void outputHexChunk(int start, unsigned char  size, FILE* fd)
{
	int addr = start;
	fprintf(fd, ":%02x%04x00", size,addr);

	int checksum = size + (unsigned char)((addr & 0xff00)>>8) + (unsigned char)(addr & 0xff);
	
	for (int i = 0; i < size; ++i)
	{
		checksum += (unsigned char)out->buffer[start + i];
		fprintf(fd, "%02x", (unsigned char)out->buffer[start + i]);
	}

	checksum = ~checksum + 1;
	fprintf(fd, "%02x\n", checksum & 0xff);
}

// output to Intel HEX
void outputHex(intval_t start, intval_t amount, FILE* fd)
{
	// max # of empty (fill) character before we start a new chunk
	const int maxEmpty							 = 32;

	// maximum bytes per line
	const unsigned char maxChunkSize = 64;

	out->buffer;

	int chunkStart = start;
	int emptyCount = 0;
	int i = start;
	for (; i < amount; ++i)
	{
		char c = out->buffer[i];
		if (out->fill_value && c == out->fill_value)
		{
			++emptyCount;
			continue;
		}

		int chunkSize = i - chunkStart;

		if (emptyCount > maxEmpty)
		{
			chunkSize -= emptyCount;
		}
		else
		{
			emptyCount = 0;
		}

		if (emptyCount || chunkSize >= maxChunkSize)
		{
			int bytesToWrite = chunkSize > maxChunkSize ? maxChunkSize : chunkSize;

			outputHexChunk(chunkStart, bytesToWrite, fd);
			chunkStart = i - (chunkSize - bytesToWrite);
		}

		emptyCount = 0;
	}

	unsigned char  chunkSize = i - chunkStart;
	if (chunkSize)
	{
		outputHexChunk(chunkStart, chunkSize, fd);
	}
	fputs(":00000001ff", fd);
}


// dump used portion of output buffer into output file
void Output_save_file(FILE *fd)
{
	intval_t	start,
			amount;

	if (out->highest_written < out->lowest_written) {
		// nothing written
		start = 0;	// I could try to use some segment start, but what for?
		amount = 0;
	} else {
		start = out->lowest_written;
		amount = out->highest_written - start + 1;
	}
	if (config.process_verbosity)
		printf("Saving %ld (0x%lx) bytes (0x%lx - 0x%lx exclusive).\n",
			amount, amount, start, start + amount);
	// output file header according to file format
	switch (output_format) {
	case OUTPUT_FORMAT_APPLE:
		PLATFORM_SETFILETYPE_APPLE(output_filename);
		// output 16-bit load address in little-endian byte order
		putc(start & 255, fd);
		putc(start >> 8, fd);
		// output 16-bit length in little-endian byte order
		putc(amount & 255, fd);
		putc(amount >> 8, fd);
		break;
	case OUTPUT_FORMAT_UNSPECIFIED:
	case OUTPUT_FORMAT_PLAIN:
		PLATFORM_SETFILETYPE_PLAIN(output_filename);
		break;
	case OUTPUT_FORMAT_CBM:
		PLATFORM_SETFILETYPE_CBM(output_filename);
		// output 16-bit load address in little-endian byte order
		putc(start & 255, fd);
		putc(start >> 8, fd);
		break;
	case OUTPUT_FORMAT_HEX:
		PLATFORM_SETFILETYPE_HEX(output_filename);
		outputHex(start, start + amount, fd);
		return;
	}
	// dump output buffer to file
	fwrite(out->buffer + start, amount, 1, fd);
}


// link segment data into segment ring
static void link_segment(intval_t start, intval_t length)
{
	struct segment	*new_segment,
			*test_segment	= out->segment.list_head.next;

	// init new segment
	new_segment = safe_malloc(sizeof(*new_segment));
	new_segment->start = start;
	new_segment->length = length;
	// use ring head as sentinel
	out->segment.list_head.start = start;
	out->segment.list_head.length = length + 1;	// +1 to make sure sentinel exits loop
	// walk ring to find correct spot
	while ((test_segment->start < new_segment->start)
	|| ((test_segment->start == new_segment->start) && (test_segment->length < new_segment->length)))
		test_segment = test_segment->next;
	// link into ring
	new_segment->next = test_segment;
	new_segment->prev = test_segment->prev;
	new_segment->next->prev = new_segment;
	new_segment->prev->next = new_segment;
}


// check whether given PC is inside segment.
// only call in first pass, otherwise too many warnings might be thrown	(TODO - still?)
static void check_segment(intval_t new_pc)
{
	struct segment	*test_segment	= out->segment.list_head.next;

	// use list head as sentinel
	out->segment.list_head.start = new_pc + 1;	// +1 to make sure sentinel exits loop
	out->segment.list_head.length = 1;
	// search ring for matching entry
	while (test_segment->start <= new_pc) {
		if ((test_segment->start + test_segment->length) > new_pc) {
			// TODO - include overlap size in error message!
			if (config.segment_warning_is_error)
				Throw_error("Segment starts inside another one, overwriting it.");
			else
				Throw_warning("Segment starts inside another one, overwriting it.");
			return;
		}

		test_segment = test_segment->next;
	}
}


// clear segment list and disable output
void Output_passinit(void)
{
//	struct segment	*temp;

//FIXME - why clear ring list in every pass?
// Because later pass shouldn't complain about overwriting the same segment from earlier pass!
// Currently this does not happen because segment warnings are only generated in first pass. FIXME!
	// delete segment list (and free blocks)
//	while ((temp = segment_list)) {
//		segment_list = segment_list->next;
//		free(temp);
//	}

	// invalidate start and end (first byte actually written will fix them)
	out->lowest_written = out->bufsize - 1;
	out->highest_written = 0;
	// deactivate output - any byte written will trigger error:
	Output_byte = no_output;
	out->write_idx = 0;	// same as pc on pass init!
	out->segment.start = NO_SEGMENT_START;	// TODO - "no active segment" could be made a segment flag!
	out->segment.max = out->bufsize - 1;	// TODO - use end of bank?
	out->segment.flags = 0;
	out->xor = 0;

	//vcpu stuff:
	CPU_state.pc.ntype = NUMTYPE_UNDEFINED;	// not defined yet
	CPU_state.pc.flags = 0;
	// FIXME - number type is "undefined", but still the intval 0 below will
	// be used to calculate diff when pc is first set.
	CPU_state.pc.val.intval = 0;	// same as output's write_idx on pass init
	CPU_state.add_to_pc = 0;	// increase PC by this at end of statement

	// pseudopc stuff:
	pseudopc_current_context = NULL;
}


// show start and end of current segment
// called whenever a new segment begins, and at end of pass.
void Output_end_segment(void)
{
	intval_t	amount;

	// in later passes, ignore completely
	if (!FIRST_PASS)
		return;

	// if there is no segment, there is nothing to do
	if (out->segment.start == NO_SEGMENT_START)
		return;

	// ignore "invisible" segments
	if (out->segment.flags & SEGMENT_FLAG_INVISIBLE)
		return;

	// ignore empty segments
	amount = out->write_idx - out->segment.start;
	if (amount == 0)
		return;

	// link to segment list
	link_segment(out->segment.start, amount);
	// announce
	if (config.process_verbosity > 1)
		// TODO - change output to start, limit, size, name:
		// TODO - output hex numbers as %04x? What about limit 0x10000?
		printf("Segment size is %ld (0x%lx) bytes (0x%lx - 0x%lx exclusive).\n",
			amount, amount, out->segment.start, out->write_idx);
}


// change output pointer and enable output
// TODO - this only gets called from vcpu_set_pc so could be made static!
void Output_start_segment(intval_t address_change, bits segment_flags)
{
	// properly finalize previous segment (link to list, announce)
	Output_end_segment();

	// calculate start of new segment
	out->write_idx = (out->write_idx + address_change) & (out->bufsize - 1);
	out->segment.start = out->write_idx;
	out->segment.flags = segment_flags;
	// allow writing to output buffer
	Output_byte = real_output;
	// in first pass, check for other segments and maybe issue warning
	// TODO - remove FIRST_PASS condition
	if (FIRST_PASS) {
		if (!(segment_flags & SEGMENT_FLAG_OVERLAY))
			check_segment(out->segment.start);
		find_segment_max(out->segment.start);
	}
}


char output_get_xor(void)
{
	return out->xor;
}
void output_set_xor(char xor)
{
	out->xor = xor;
}


// set program counter to defined value (FIXME - allow for undefined!)
// if start address was given on command line, main loop will call this before each pass.
// in addition to that, it will be called on each "*= VALUE".
void vcpu_set_pc(intval_t new_pc, bits segment_flags)
{
	intval_t	pc_change;

	// support stupidly bad, old, ancient, deprecated, obsolete behaviour:
	if (pseudopc_current_context != NULL) {
		if (config.wanted_version < VER_SHORTER_SETPC_WARNING) {
			Throw_warning("Offset assembly still active at end of segment. Switched it off.");
			pseudopc_end_all();
		} else if (config.wanted_version < VER_DISABLED_OBSOLETE_STUFF) {
			Throw_warning("Offset assembly still active at end of segment.");
			pseudopc_end_all();	// warning no longer said it
			// would switch off, but still did. nevertheless, there
			// is something different to older versions: when the
			// closing '}' or !realpc is encountered, _really_ weird
			// stuff happens! i see no reason to try to mimic that.
		}
	}
	pc_change = new_pc - CPU_state.pc.val.intval;
	CPU_state.pc.val.intval = new_pc;	// FIXME - oversized values are accepted without error and will be wrapped at end of statement!
	CPU_state.pc.ntype = NUMTYPE_INT;	// FIXME - remove when allowing undefined!
	CPU_state.pc.addr_refs = 1;	// yes, PC counts as address
	// now tell output buffer to start a new segment
	Output_start_segment(pc_change, segment_flags);
}
/*
TODO - overhaul program counter and memory pointer stuff:
general stuff: PC and mem ptr might be marked as "undefined" via flags field.
However, their "value" fields are still updated, so we can calculate differences.

on pass init:
	if value given on command line, set PC and out ptr to that value
	otherwise, set both to zero and mark as "undefined"
when ALU asks for "*":
	return current PC (value and flags)
when encountering "!pseudopc VALUE { BLOCK }":
	parse new value (NEW: might be undefined!)
	remember difference between current and new value
	set PC to new value
	after BLOCK, use remembered difference to change PC back
when encountering "*= VALUE":
	parse new value (NEW: might be undefined!)
	calculate difference between current PC and new value
	set PC to new value
	tell outbuf to add difference to mem ptr (starting a new segment) - if new value is undefined, tell outbuf to disable output

Problem: always check for "undefined"; there are some problematic combinations.
I need a way to return the size of a generated code block even if PC undefined.
Maybe like this:
	*= new_address [, invisible] [, overlay] [, &size_symbol_ref {]
		...code...
	[} ; at end of block, size is written to size symbol given above!]
*/


// get program counter
void vcpu_read_pc(struct number *target)
{
	*target = CPU_state.pc;
}


// get size of current statement (until now) - needed for "!bin" verbose output
int vcpu_get_statement_size(void)
{
	return CPU_state.add_to_pc;
}


// adjust program counter (called at end of each statement)
void vcpu_end_statement(void)
{
	CPU_state.pc.val.intval = (CPU_state.pc.val.intval + CPU_state.add_to_pc) & (out->bufsize - 1);
	CPU_state.add_to_pc = 0;
}


// struct to describe a pseudopc context
struct pseudopc {
	struct pseudopc	*outer;	// next layer (to be able to "unpseudopc" labels by more than one level)
	intval_t	offset;	// inner minus outer pc
	enum numtype	ntype;	// type of outer pc (INT/UNDEFINED)
};
// start offset assembly
void pseudopc_start(struct number *new_pc)
{
	struct pseudopc	*new_context;

	new_context = safe_malloc(sizeof(*new_context));	// create new struct (this must never be freed, as it gets linked to labels!)
	new_context->outer = pseudopc_current_context;	// let it point to previous one
	pseudopc_current_context = new_context;	// make it the current one

	new_context->ntype = CPU_state.pc.ntype;
	new_context->offset = new_pc->val.intval - CPU_state.pc.val.intval;
	CPU_state.pc.val.intval = new_pc->val.intval;
	CPU_state.pc.ntype = NUMTYPE_INT;	// FIXME - remove when allowing undefined!
	//new: CPU_state.pc.flags = new_pc->flags & (NUMBER_IS_DEFINED | NUMBER_EVER_UNDEFINED);
}
// end offset assembly
void pseudopc_end(void)
{
	if (pseudopc_current_context == NULL) {
		// trying to end offset assembly though it isn't active:
		// in current versions this cannot happen and so must be a bug.
		// but in versions older than 0.94.8 this was possible using
		// !realpc, and offset assembly got automatically disabled when
		// encountering "*=".
		// so if wanted version is new enough, choke on bug!
		if (config.wanted_version >= VER_DISABLED_OBSOLETE_STUFF)
			Bug_found("ClosingUnopenedPseudopcBlock", 0);
	} else {
		CPU_state.pc.val.intval = (CPU_state.pc.val.intval - pseudopc_current_context->offset) & (out->bufsize - 1);	// pc might have wrapped around
		CPU_state.pc.ntype = pseudopc_current_context->ntype;
		pseudopc_current_context = pseudopc_current_context->outer;	// go back to outer block
	}
}
// this is only for old, deprecated, obsolete, stupid "realpc":
void pseudopc_end_all(void)
{
	while (pseudopc_current_context)
		pseudopc_end();
}
// un-pseudopc a label value by given number of levels
// returns nonzero on error (if level too high)
int pseudopc_unpseudo(struct number *target, struct pseudopc *context, unsigned int levels)
{
	while (levels--) {
		//if (target->ntype == NUMTYPE_UNDEFINED)
		//	return 0;	// ok (no sense in trying to unpseudo this, and it might be an unresolved forward ref anyway)

		if (context == NULL) {
			Throw_error("Un-pseudopc operator '&' has no !pseudopc context.");
			return 1;	// error
		}
		// FIXME - in future, check both target and context for NUMTYPE_UNDEFINED!
		target->val.intval = (target->val.intval - context->offset) & (out->bufsize - 1);	// FIXME - is masking really needed?	TODO
		context = context->outer;
	}
	return 0;	// ok
}
// return pointer to current "pseudopc" struct (may be NULL!)
// this gets called when parsing label definitions
struct pseudopc *pseudopc_get_context(void)
{
	return pseudopc_current_context;
}
