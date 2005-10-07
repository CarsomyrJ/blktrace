/*
 * This file contains format parsing code for blkparse, allowing you to
 * customize the individual action format and generel output format.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "blktrace.h"

#define VALID_SPECS	"ABCDFGMPQRSTUWX"

#define HEADER		"%D %2c %8s %5T.%9t %5p %2a %3d "

static char *override_format[256];

static inline int valid_spec(int spec)
{
	return strchr(VALID_SPECS, spec) != NULL;
}

void set_all_format_specs(char *option)
{
	char *p;

	for (p = VALID_SPECS; *p; p++)
		if (override_format[(int)(*p)] == NULL)
			override_format[(int)(*p)] = strdup(option);
}

int add_format_spec(char *option)
{
	int spec = optarg[0];

	if (!valid_spec(spec)) {
		fprintf(stderr,"Bad format specifier %c\n", spec);
		return 1;
	}
	if (optarg[1] != ',') {
		fprintf(stderr,"Bad format specifier - need ',' %s\n", option);
		return 1;
	}
	option += 2;
	if (*option == '\0') {
		fprintf(stderr,"Bad format specifier - need fmt %s\n", option);
		return 1;
	}

	/*
	 * Set both merges (front and back)
	 */
	if (spec == 'M') {
		override_format['B'] = strdup(option);
		override_format['M'] = strdup(option);
	} else
		override_format[spec] = strdup(option);

	return 0;
}

static inline void fill_rwbs(char *rwbs, struct blk_io_trace *t)
{
	int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);
	int b = t->action & BLK_TC_ACT(BLK_TC_BARRIER);
	int s = t->action & BLK_TC_ACT(BLK_TC_SYNC);
	int i = 0;

	if (w)
		rwbs[i++] = 'W';
	else
		rwbs[i++] = 'R';
	if (b)
		rwbs[i++] = 'B';
	if (s)
		rwbs[i++] = 'S';

	rwbs[i] = '\0';
}

static int pdu_rest_is_zero(unsigned char *pdu, int len)
{
	int i = 0;

	while (!pdu[i] && i < len)
		i++;

	return i == len;
}

static char *dump_pdu(unsigned char *pdu_buf, int pdu_len)
{
	static char p[4096];
	int i, len;

	if (!pdu_buf || !pdu_len)
		return NULL;

	for (len = 0, i = 0; i < pdu_len; i++) {
		if (i)
			len += sprintf(p + len, " ");

		len += sprintf(p + len, "%02x", pdu_buf[i]);

		/*
		 * usually dump for cdb dumps where we can see lots of
		 * zeroes, stop when the rest is just zeroes and indicate
		 * so with a .. appended
		 */
		if (!pdu_buf[i] && pdu_rest_is_zero(pdu_buf + i, pdu_len - i)) {
			sprintf(p + len, " ..");
			break;
		}
	}

	return p;
}

#define pdu_start(t)	(((void *) (t) + sizeof(struct blk_io_trace)))

static unsigned int get_pdu_int(struct blk_io_trace *t)
{
	__u64 *val = pdu_start(t);

	return be64_to_cpu(*val);
}

static void get_pdu_remap(struct blk_io_trace *t, struct blk_io_trace_remap *r)
{
	struct blk_io_trace_remap *__r = pdu_start(t);

	r->device = be32_to_cpu(__r->device);
	r->sector = be64_to_cpu(__r->sector);
}

static void print_field(char *act, struct per_cpu_info *pci,
			struct blk_io_trace *t, unsigned long long elapsed,
			int pdu_len, unsigned char *pdu_buf, char field,
			int minus, int has_w, int width)
{
	char format[64];

	if (has_w) {
		if (minus)
			sprintf(format, "%%-%d", width);
		else
			sprintf(format, "%%%d", width);
	} else
		sprintf(format, "%%");

	switch (field) {
	case 'a':
		fprintf(ofp, strcat(format, "s"), act);
		break;
	case 'c':
		fprintf(ofp, strcat(format, "d"), pci->cpu);
		break;
	case 'C':
		fprintf(ofp, strcat(format, "s"), t->comm);
		break;
	case 'd': {
		char rwbs[4];

		fill_rwbs(rwbs, t);
		fprintf(ofp, strcat(format, "s"), rwbs);
		break;
	}
	case 'D':	/* format width ignored */
		fprintf(ofp,"%3d,%-3d", MAJOR(t->device), MINOR(t->device));
		break;
	case 'e':
		fprintf(ofp, strcat(format, "d"), t->error);
		break;
	case 'M':
		fprintf(ofp, strcat(format, "d"), MAJOR(t->device));
		break;
	case 'm':
		fprintf(ofp, strcat(format, "d"), MINOR(t->device));
		break;
	case 'n':
		fprintf(ofp, strcat(format, "u"), t_sec(t));
		break;
	case 'N':
		fprintf(ofp, strcat(format, "u"), t->bytes);
		break;
	case 'p':
		fprintf(ofp, strcat(format, "u"), t->pid);
		break;
	case 'P': { /* format width ignored */
		char *p = dump_pdu(pdu_buf, pdu_len);
		if (p)
			fprintf(ofp, "%s", p);
		break;
	}
	case 's':
		fprintf(ofp, strcat(format, "ld"), t->sequence);
		break;
	case 'S':
		fprintf(ofp, strcat(format, "lu"), t->sector);
		break;
	case 't':
		sprintf(format, "%%0%dlu", has_w ? width : 9);
		fprintf(ofp, format, NANO_SECONDS(t->time));
		break;
	case 'T':
		fprintf(ofp, strcat(format, "d"), SECONDS(t->time));
		break;
	case 'u':
		if (elapsed == -1ULL) {
			fprintf(stderr, "Expecting elapsed value\n");
			exit(1);
		}
		fprintf(ofp, strcat(format, "llu"), elapsed / 1000);
		break;
	case 'U': {
		fprintf(ofp, strcat(format, "u"), get_pdu_int(t));
		break;
	}
	default:
		fprintf(ofp,strcat(format, "c"), field);
		break;
	}
}

static char *parse_field(char *act, struct per_cpu_info *pci,
			 struct blk_io_trace *t, unsigned long long elapsed,
			 int pdu_len, unsigned char *pdu_buf,
			 char *master_format)
{
	int minus = 0;
	int has_w = 0;
	int width = 0;
	char *p = master_format;

	if (*p == '-') {
		minus = 1;
		p++;
	}
	if (isdigit(*p)) {
		has_w = 1;
		do {
			width = (width * 10) + (*p++ - '0');
		} while ((*p) && (isdigit(*p)));
	}
	if (*p) {
		print_field(act, pci, t, elapsed, pdu_len, pdu_buf, *p++,
			    minus, has_w, width);
	}
	return p;
}

static void process_default(char *act, struct per_cpu_info *pci,
			    struct blk_io_trace *t, unsigned long long elapsed,
			    int pdu_len, unsigned char *pdu_buf)
{
	char rwbs[4];

	fill_rwbs(rwbs, t);

	/*
	 * The header is always the same
	 */
	fprintf(ofp, "%3d,%-3d %2d %8d %5d.%09lu %5u %2s %3s ",
		MAJOR(t->device), MINOR(t->device), pci->cpu, t->sequence,
		(int) SECONDS(t->time), (unsigned long) NANO_SECONDS(t->time),
		t->pid, act, rwbs);

	switch (act[0]) {
	case 'C': 	/* Complete */
		if (t->action & BLK_TC_ACT(BLK_TC_PC)) {
			char *p = dump_pdu(pdu_buf, pdu_len);
			if (p)
				fprintf(ofp, "(%s) ", p);
			fprintf(ofp, "[%d]\n", t->error);
		} else {
			if (elapsed != -1ULL) {
				fprintf(ofp, "%llu + %u (%8llu) [%d]\n",
					(unsigned long long) t->sector,
					t_sec(t), elapsed, t->error);
			} else {
				fprintf(ofp, "%llu + %u [%d]\n",
					(unsigned long long) t->sector,
					t_sec(t), t->error);
			}
		}
		break;

	case 'D': 	/* Issue */
	case 'I': 	/* Insert */
	case 'Q': 	/* Queue */
	case 'W':	/* Bounce */
		if (t->action & BLK_TC_ACT(BLK_TC_PC)) {
			char *p;
			fprintf(ofp, "%u ", t->bytes);
			p = dump_pdu(pdu_buf, pdu_len);
			if (p)
				fprintf(ofp, "(%s) ", p);
			fprintf(ofp, "[%s]\n", t->comm);
		} else {
			if (elapsed != -1ULL) {
				fprintf(ofp, "%llu + %u (%8llu) [%s]\n",
					(unsigned long long) t->sector,
					t_sec(t), elapsed, t->comm);
			} else {
				fprintf(ofp, "%llu + %u [%s]\n",
					(unsigned long long) t->sector,
					t_sec(t), t->comm);
			}
		}
		break;

	case 'B':	/* Back merge */
	case 'F':	/* Front merge */
	case 'M':	/* Front or back merge */
	case 'G':	/* Get request */
	case 'S':	/* Sleep request */
		fprintf(ofp, "%llu + %u [%s]\n", (unsigned long long) t->sector,
			t_sec(t), t->comm);
		break;

	case 'P':	/* Plug */
		fprintf(ofp, "[%s]\n", t->comm);
		break;

	case 'U':	/* Unplug IO */
	case 'T': 	/* Unplug timer */
		fprintf(ofp, "[%s] %u\n", t->comm, get_pdu_int(t));
		break;

	case 'A': {	/* remap */
		struct blk_io_trace_remap r;

		get_pdu_remap(t, &r);
		fprintf(ofp, "%llu + %u <- (%d,%d) %llu\n",
			(unsigned long long) r.sector, t_sec(t),
			MAJOR(r.device), MINOR(r.device),
			(unsigned long long) t->sector);
		break;
	}
		
	case 'X': 	/* Split */
		fprintf(ofp, "%llu / %u [%s]\n", (unsigned long long) t->sector,
			get_pdu_int(t), t->comm);
		break;

	default:
		fprintf(stderr, "Unknown action %c\n", act[0]);
		break;
	}

}

void process_fmt(char *act, struct per_cpu_info *pci, struct blk_io_trace *t,
		 unsigned long long elapsed, int pdu_len,
		 unsigned char *pdu_buf)
{
	char *p = override_format[(int) *act];

	if (!p) {
		process_default(act, pci, t, elapsed, pdu_len, pdu_buf);
		return;
	}

	while (*p) {
		switch (*p) {
		case '%': 	/* Field specifier */
			p++;
			if (*p == '%')
				fprintf(ofp, "%c", *p++);
			else if (!*p)
				fprintf(ofp, "%c", '%');
			else
				p = parse_field(act, pci, t, elapsed,
						pdu_len, pdu_buf, p);
			break;
		case '\\': {	/* escape */
			switch (p[1]) {
			case 'b': fprintf(ofp, "\b"); break;
			case 'n': fprintf(ofp, "\n"); break;
			case 'r': fprintf(ofp, "\r"); break;
			case 't': fprintf(ofp, "\t"); break;
			default:
				fprintf(stderr,	
					"Invalid escape char in format %c\n",
					p[1]);
				exit(1);
				/*NOTREACHED*/
			}
			p += 2;
			break;
		}
		default:
			fprintf(ofp, "%c", *p++);
			break;
		}
	}
}


