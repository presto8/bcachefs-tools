/*
 * Authors: Kent Overstreet <kent.overstreet@gmail.com>
 *	    Gabriel de Perthuis <g2p.code@gmail.com>
 *	    Jacob Malevich <jam@datera.io>
 *
 * GPLv2
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "ccan/darray/darray.h"

#include "cmds.h"
#include "libbcachefs.h"
#include "crypto.h"
#include "libbcachefs/opts.h"
#include "libbcachefs/super-io.h"
#include "libbcachefs/util.h"

#define OPTS						\
x(0,	replicas,		required_argument)	\
x(0,	encrypted,		no_argument)		\
x(0,	no_passphrase,		no_argument)		\
x('L',	label,			required_argument)	\
x('U',	uuid,			required_argument)	\
x(0,	fs_size,		required_argument)	\
x(0,	bucket_size,		required_argument)	\
x('g',	group,			required_argument)	\
x(0,	discard,		no_argument)		\
x(0,	data_allowed,		required_argument)	\
x(0,	durability,		required_argument)	\
x('f',	force,			no_argument)		\
x('q',	quiet,			no_argument)		\
x('h',	help,			no_argument)

static void usage(void)
{
	puts("bcachefs format - create a new bcachefs filesystem on one or more devices\n"
	     "Usage: bcachefs format [OPTION]... <devices>\n"
	     "\n"
	     "Options:");

	bch2_opts_usage(OPT_FORMAT);

	puts(
	     "      --replicas=#            Sets both data and metadata replicas\n"
	     "      --encrypted             Enable whole filesystem encryption (chacha20/poly1305)\n"
	     "      --no_passphrase         Don't encrypt master encryption key\n"
	     "  -L, --label=label\n"
	     "  -U, --uuid=uuid\n"
	     "\n"
	     "Device specific options:");

	bch2_opts_usage(OPT_DEVICE);

	puts("  -g, --group=label           Disk group\n"
	     "\n"
	     "  -f, --force\n"
	     "  -q, --quiet                 Only print errors\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Device specific options must come before corresponding devices, e.g.\n"
	     "  bcachefs format --group cache /dev/sdb /dev/sdc\n"
	     "\n"
	     "Report bugs to <linux-bcache@vger.kernel.org>");
}

enum {
	O_no_opt = 1,
#define x(shortopt, longopt, arg)	O_##longopt,
	OPTS
#undef x
};

#define x(shortopt, longopt, arg) {			\
	.name		= #longopt,			\
	.has_arg	= arg,				\
	.flag		= NULL,				\
	.val		= O_##longopt,			\
},
static const struct option format_opts[] = {
	OPTS
	{ NULL }
};
#undef x

u64 read_flag_list_or_die(char *opt, const char * const list[],
			  const char *msg)
{
	u64 v = bch2_read_flag_list(opt, list);
	if (v == (u64) -1)
		die("Bad %s %s", msg, opt);

	return v;
}

int cmd_format(int argc, char *argv[])
{
	darray(struct dev_opts) devices;
	struct format_opts opts	= format_opts_default();
	struct dev_opts dev_opts = dev_opts_default(), *dev;
	bool force = false, no_passphrase = false, quiet = false;
	unsigned v;
	int opt;

	darray_init(devices);

	struct bch_opt_strs fs_opt_strs =
		bch2_cmdline_opts_get(&argc, argv, OPT_FORMAT);
	struct bch_opts fs_opts = bch2_parse_opts(fs_opt_strs);

	while ((opt = getopt_long(argc, argv,
				  "-L:U:fqh",
				  format_opts,
				  NULL)) != -1)
		switch (opt) {
		case O_replicas:
			if (kstrtouint(optarg, 10, &v) ||
			    !v ||
			    v > BCH_REPLICAS_MAX)
				die("invalid replicas");

			opt_set(fs_opts, metadata_replicas, v);
			opt_set(fs_opts, data_replicas, v);
			break;
		case O_encrypted:
			opts.encrypted = true;
			break;
		case O_no_passphrase:
			no_passphrase = true;
			break;
		case O_label:
		case 'L':
			opts.label = optarg;
			break;
		case O_uuid:
		case 'U':
			if (uuid_parse(optarg, opts.uuid.b))
				die("Bad uuid");
			break;
		case O_force:
		case 'f':
			force = true;
			break;
		case O_fs_size:
			if (bch2_strtoull_h(optarg, &dev_opts.size))
				die("invalid filesystem size");

			dev_opts.size >>= 9;
			break;
		case O_bucket_size:
			dev_opts.bucket_size =
				hatoi_validate(optarg, "bucket size");
			break;
		case O_group:
		case 'g':
			dev_opts.group = optarg;
			break;
		case O_discard:
			dev_opts.discard = true;
			break;
		case O_data_allowed:
			dev_opts.data_allowed =
				read_flag_list_or_die(optarg,
					bch2_data_types, "data type");
			break;
		case O_durability:
			if (kstrtouint(optarg, 10, &dev_opts.durability) ||
			    dev_opts.durability > BCH_REPLICAS_MAX)
				die("invalid durability");
			break;
		case O_no_opt:
			dev_opts.path = optarg;
			darray_append(devices, dev_opts);
			dev_opts.size = 0;
			break;
		case O_quiet:
		case 'q':
			quiet = true;
			break;
		case O_help:
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
			break;
		case '?':
			die("unrecognized option %s", optarg);
			break;
		}

	if (darray_empty(devices))
		die("Please supply a device");

	if (opts.encrypted && !no_passphrase)
		opts.passphrase = read_passphrase_twice("Enter passphrase: ");

	darray_foreach(dev, devices)
		dev->fd = open_for_format(dev->path, force);

	struct bch_sb *sb =
		bch2_format(fs_opt_strs,
			    fs_opts,
			    opts,
			    devices.item, darray_size(devices));

	if (!quiet)
		bch2_sb_print(sb, false, 1 << BCH_SB_FIELD_members, HUMAN_READABLE);
	free(sb);

	if (opts.passphrase) {
		memzero_explicit(opts.passphrase, strlen(opts.passphrase));
		free(opts.passphrase);
	}

	darray_free(devices);

	return 0;
}

static void show_super_usage(void)
{
	puts("bcachefs show-super \n"
	     "Usage: bcachefs show-super [OPTION].. device\n"
	     "\n"
	     "Options:\n"
	     "  -f, --fields=(fields)       list of sections to print\n"
	     "  -l, --layout                print superblock layout\n"
	     "  -h, --help                  display this help and exit\n"
	     "Report bugs to <linux-bcache@vger.kernel.org>");
	exit(EXIT_SUCCESS);
}

int cmd_show_super(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "fields",			1, NULL, 'f' },
		{ "layout",			0, NULL, 'l' },
		{ "help",			0, NULL, 'h' },
		{ NULL }
	};
	unsigned fields = 1 << BCH_SB_FIELD_members;
	bool print_layout = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "f:lh", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			fields = !strcmp(optarg, "all")
				? ~0
				: read_flag_list_or_die(optarg,
					bch2_sb_fields, "superblock field");
			break;
		case 'l':
			print_layout = true;
			break;
		case 'h':
			show_super_usage();
			break;
		}
	args_shift(optind);

	char *dev = arg_pop();
	if (!dev)
		die("please supply a device");
	if (argc)
		die("too many arguments");

	struct bch_opts opts = bch2_opts_empty();

	opt_set(opts, noexcl,	true);
	opt_set(opts, nochanges, true);

	struct bch_sb_handle sb;
	int ret = bch2_read_super(dev, &opts, &sb);
	if (ret)
		die("Error opening %s: %s", dev, strerror(-ret));

	bch2_sb_print(sb.sb, print_layout, fields, HUMAN_READABLE);
	bch2_free_super(&sb);
	return 0;
}
